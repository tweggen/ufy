# Unify Engine — Roadmap to a Stable Language PoC

Status: draft, created 2026-08-20 from a code review of the Unify engine
(then living at `combine/modules/unify` in the vault repository).

Unify is a Prolog-family logic language with C-like surface syntax (`.ufy`),
embedded as the rule engine of the vault home-automation system. This document
tracks the work needed to (A) make it a stable, functioning programming-language
PoC and (B) make the engine solidly multithreaded and asynchronous.

Tick items off as they land (`[x]`). Phases are ordered by dependency: each
phase assumes the previous one is substantially done. Phases 0–2 are the PoC
bar; Phases 3–4 make it pleasant; Phase 5 is the concurrency track.

---

## Current state (summary of findings)

Architecture that exists and works:

- Boost.Spirit parser → AST → term trees (`ConsTerm`/`VarTerm`/`MapTerm`,
  double-dispatch unification).
- Persistent binding environments: bindings live in a tree of `UnifyContext`
  objects (`(UnifyContextId, VarTermId) → InstanceId → SingleVarInstance`),
  so backtracking needs no trail. Elegant, keep it.
- Explicit-stack depth-first solver (`SolveJob` / `SolveContext` / `GoalPart`
  continuation chains).
- Engine with job queues (ready/blocked/zombie), worker loop, debugger
  (xdebug-style TCP).

Sketched but not functional (these drive the roadmap):

- Slicing: `performSlice()` runs the whole query in one "slice"; the blocked
  queue and `ClauseContinuationContext` (async unification) are never exercised.
- Memory: raw `new` throughout; `UnifyContext`s, `GoalPart`s and terms leak;
  solution contexts have undefined lifetime; debug `abort()` in `discardTop()`.
- Thread-safety: unsynchronized static id counters (`UnifyContext::m_iidLast`,
  `uidNextUnifyContext`, `VarTerm::m_counterUidTerm`, `Clause::m_nextUid`,
  `Job::m_idNextJob`); `Job::state()` unlocked; `ExecutionState::appendClause`
  locking commented out; `World::setTermDebugInfo` unlocked; no engine
  shutdown path; `removeUserEventListener` unimplemented.
- Errors: parse failure prints `"Parse error."` without location (Spirit
  `on_error` handlers are `#if 0`'d); `UnifyError` silently becomes "no match".
- Language gaps: no arithmetic/comparison, no cut, no findall/aggregation, no
  runtime assert/retract, no imports; solutions returned only as strings.
- Hygiene: no automated tests; `.cpp~` backup files committed; C++03-era
  boost; Linux-only Boost.Jam build via `BOB*` env vars; clause lookup is a
  linear scan of the whole database per goal term.

---

## Phase 0 — Reproducible baseline (make it honest)

Goal: build cleanly and reproducibly, and put a safety net under all later
refactoring. Nothing else starts before the test harness exists.

- [x] Port the build to CMake (or pin the exact boost version for Boot.Jam);
      dependencies via vcpkg or Conan.
      *(2026-08-20: `CMakeLists.txt` with `vault-unify-core` static lib,
      the `UNIFY_BUILD_XDEBUG` option, and a new
      `tools/unify-run.cpp` CLI runner. Boost comes from the system package
      in CI for now; compile validation happens on the Linux CI job below —
      the Windows dev machine has no Boost toolchain.)*
- [x] Build clean on a modern compiler with `-Wall -Wextra`; fix or triage
      every warning. *(Zero warnings on Ubuntu/GCC as of 2026-08-20.)*
- [x] Remove committed backup files (`*.cpp~`, `*.ufy~`) and add them to
      `.gitignore`.
- [x] Golden-output test harness: run a `.ufy` program, diff emitted solutions
      against an expected file. Seed from `test-engine.ufy`, `test2.ufy`,
      `test/mapsyntax.ufy`, `pathfinder.ufy`.
      *(`test/run-golden-test.sh` + CTest, `UNIFY_UPDATE_GOLDEN=1`
      regeneration. The seeded golden files are empty because none of the
      four sample programs contains a query — they pin "parses cleanly,
      runs, exits, no output" for now, which already catches hangs and
      crashes. Programs with real queries/solutions are Phase 2's "restore
      the example programs" item.)*
- [x] Wire the harness into CI (even a single GitHub Actions job on Linux).
      *(`.github/workflows/unify-ci.yml`: Ubuntu + apt Boost, build, ctest,
      sample-output artifact for golden-file bootstrapping.)*
- [x] Remove the debug `abort()` in `SolveJob::discardTop()`; return/log an
      internal error instead.
- [x] *(Found during the port)* Fix `Engine::addWorkerThread()` destroying a
      joinable `boost::thread` (would `std::terminate()` with modern Boost);
      threads are now kept in `Engine::m_lsWorkerThreads`, to be joined once
      a shutdown path exists (Phase 5.1).
- [x] *(Found by the first golden runs)* `Engine::Engine()` left
      `m_isDebugHalted` (and `m_pDebugListener`) uninitialized;
      `executionLoop()` gates on that bool, so depending on heap layout —
      which varied with the size of the loaded `.ufy` file — the scheduler
      slept forever and larger programs (`test2`, `pathfinder`) deadlocked
      while small ones ran. Diagnosed via gdb-in-CI; both members are now
      initialized.

## Phase 1 — Correctness core

Goal: defined semantics, defined ownership, real error reporting.

- [x] **Ownership model**: per-`SolveJob` arena/region that owns all
      `UnifyContext`s, `GoalPart`s and terms instantiated during solving;
      freed when the job's results are consumed. (This legitimizes the current
      allocate-freely style instead of fighting it.)
      *(2026-08-20, passes 1+2 landed: per-job arena frees all solve-time
      UnifyContexts/GoalParts/Goal objects at job end (finished jobs now
      actually destruct); `~World` frees the whole clause database, debug
      info, and builtin heads via a de-duplicating term sweep. Remaining
      leaks per program are (a) parser-side orphan terms — intermediates
      built during desugaring and then replaced, never owned (e.g. test2
      14,065 B/523 allocs ≈ its pre-teardown baseline, i.e. the clause DB
      itself is fully freed) — and (b) *(resolved)* per-query goal term
      trees: freed by `~SolveJob` since the if-statement rework removed the
      aliasing (conformance programs dropped to 360–975 B). Reaching zero
      now only needs the parser-orphan cleanup: intermediate terms built
      during AST→term conversion and then replaced (e.g. by the `->`
      desugaring) that no owner ever adopts. DONE 2026-08-20: the orphans
      were an unfreed heap Atom and transient `new T*[n]` argument arrays;
      all seven sample/conformance programs now run with ZERO LeakSanitizer
      findings, and the CI leak check gates on staying at zero.)*
- [x] Run the test suite under ASan/LeakSanitizer; zero leaks per query.
      *(2026-08-20: `UNIFY_SANITIZE` CMake option + a `sanitize` CI job now
      build and run the golden-output suite under ASan+UBSan with leak
      detection disabled, gating CI on memory errors (use-after-free,
      overflow, UB) immediately; an informational, non-gating leak report
      (`unify-leak-report` artifact) runs the sample programs with leak
      detection on, to track progress towards the zero-leaks exit criterion.
      First green sanitize run found **zero memory errors**; leak baseline:
      test-engine 1,312 B/49 allocs, mapsyntax 3,072 B/120,
      test2 14,129 B/524, pathfinder 19,497 B/745. Reaching zero leaks
      still requires the Ownership model item above.)*
- [x] Re-enable Spirit `on_error` handlers; parse errors report file, line,
      column and the offending line.
      *(Implemented at the `phrase_parse` call site instead: the grammar has
      no expectation points, so `on_error<fail>` could never fire. gcc-style
      diagnostics on stderr; `parseExecuteSegment` returns the error count;
      `unify-run` exits 1 on parse errors; negative golden test
      `test/parse-error.ufy` pins the behaviour.)*
- [x] *(Found by the first conformance runs, 2026-08-20)* Fix a grammar
      ambiguity that made facts + queries in one file impossible: the query
      rule (`goal; goal; ?`) greedily swallowed every preceding
      `;`-terminated fact into the query goal, so programs defining facts
      and then querying them defined no clauses at all (confirmed via a
      full solver trace in CI). Queries are now `query { g1; g2; }` blocks
      (C/Java feel per the language owner; a comma-`?` form existed only
      transiently on the way there), making `;`-terminated top-level terms
      unambiguously clauses — no pre-existing program used queries, so
      nothing breaks. NOTE: `query` is thereby reserved as the head of a
      zero-argument rule; `query(...)` with arguments remains usable.
- [x] Propagate `UnifyError` as an error (with message) instead of mapping it
      to "did not unify".
      *(2026-08-20: masking sites fixed — including one where a subterm
      error became silent *success* in `ConsTerm`/`MapTerm` — errors are
      logged, recorded per job via `SolveJob::getErrorCount()`, and drive
      `unify-run`'s exit code. The search itself is unchanged.)*
- [x] Write a short language spec (`SPEC.md`): clause selection order,
      negation-as-failure semantics (incl. the `UnifyLast`/`UnifyNotLast`/
      `m_foundClause` rules), `if` statement, `->` deref, map/array
      unification, `=` and prefix `!`. Add a conformance test per rule.
      *(2026-08-20: SPEC.md derived from source with per-claim citations;
      8 conformance programs in `test/conformance/` with committed goldens.
      Every golden matches the behaviour predicted from code reading —
      including the documented QUIRKs (if-bodies unreachable, non-textbook
      negation on undefined predicates, arrays desugared to maps).)*
- [ ] Return solutions as structured terms (term tree / JSON), not
      `toString()` output; REST layer consumes the structured form.
- [ ] Unit tests for the binding machinery (`findVarBinding` /
      `findVarInstance` / `genericUnifyVarWithKnown`) including
      var–var chains and cross-context bindings.
- [ ] Decide on and document the occurs-check policy (Prolog default: off;
      optionally `unify_with_occurs_check`-style builtin).

## Phase 2 — Language completeness (PoC feature bar)

Goal: enough language to write real programs (the home-automation rules are
the reference workload).

- [x] Arithmetic and comparison builtins (`+ - * /`, `< > <= >= == !=`) with a
      defined evaluation construct (`is`-style or expression goals).
      *(2026-08-20: C-style expression goals — `$y = 2 + 3 * 4;` evaluates
      when a side of `=` is arithmetic (plain `=` stays unification),
      comparisons are goals with numeric-then-string semantics, int64,
      division by zero → UnifyError. SPEC.md §4.1; conformance goldens
      pin precedence, negatives, and silent comparison failure.)*
- [x] Cut (`!` in Prolog's sense) or a committed-choice construct; reconcile
      with the current prefix-`!` negation syntax.
      *(2026-08-20: the keyword goal `cut;` — prefix `!` stays negation.
      Standard clause-scoped Prolog semantics incl. query-level cut,
      implemented as a solver-level prune (clause-iterator invalidation
      from stack top through the entry context). `if` gained a cut after
      its condition and is now a true committed if-then-else. Conformance
      goldens pin all four cut behaviours.)*
- [x] `findall` / aggregation over solutions.
      *(2026-08-21: `$all = findall( $x, goal( $x ) );` — solver-level
      special form running a nested synchronous SolveJob; results
      ground-copied into a first-class array; deterministic, never fails,
      `[]` on zero solutions. v1 limitation documented in SPEC: the
      subgoal solves in a fresh scope, outer bindings not consulted.)*
- [x] Classic `for` loop and `foreach` (language owner request, 2026-08-21):
      `for ($i = 0; $i < 10; $i = $i + 1) { ... }` and
      `foreach ($x : $arr) { ... }`, desugared to synthesized recursive
      clauses the same way `if` desugars — no new solver machinery.
      *(2026-08-21: shipped incl. range literals `a..b` (eager array for
      literal bounds, `__builtin_range` for variable bounds, 100k cap).
      Semantics: foreach continues on body failure (body-or-true wrapper
      clause); for stops on cond/body failure; both commit iterations via
      cut. SPEC.md §12.)*
- [x] **Engine bug found by the loop work (2026-08-21):** threading the SAME
      `VarTerm` through a clause's own recursive call (`p($a,$i) { ...;
      p($a,$j); }` reusing `$a`) does not propagate the value past the first
      recursion — `VarTerm::unifyVarTerm`'s `this==pOther` identity fast
      path records no binding, and `AssignmentId` lookups have no
      ancestor-scope fallback. The loop desugaring works around it with
      explicit rebinding; hand-written recursive predicates hit it. Needs a
      proper fix in the binding machinery (SPEC.md §10 documents it).
- [ ] **Exploratory — constraint domains (language owner's long-term wish):**
      typed declarations (`int $a;`, `float $a;`) giving unbound variables a
      DOMAIN instead of a single binding; comparisons over unbound typed
      vars then *narrow* the domain (`$a < 10` ⇒ $a is "all ints < 10" as an
      enumerable set, or a float interval). This is constraint logic
      programming (CLP(FD)/interval-CLP): needs a domain representation in
      the binding machinery (today a binding is exactly one term), domain
      types (range, finite set), propagation on each new constraint, and
      enumeration (`foreach` over a domain / labeling). Research-grade;
      staged path: (1) range/set VALUES as data + foreach over them,
      (2) typed declarations, (3) domain-narrowing comparisons for int,
      (4) float intervals. Not started.
- [x] String operations (concat, compare, match).
      *(2026-08-21: `$s = concat(...)` (variadic) and `$n = strlen($s)` as
      `=`-position forms; `contains`/`startswith`/`endswith` goal builtins;
      comparison was already covered by the comparison operators.
      SPEC.md §14; golden-pinned.)*
- [x] Runtime `assert` / `retract` — required for device/sensor state changes.
      Implement against the versioned World (see Phase 5, item 2; the
      copy-on-write design is already described in `vault-unify.hpp`).
      *(2026-08-21: solver special forms; assertz of ground facts
      (ground-copied into World ownership), retract tombstones the first
      matching fact (non-binding, live view). Full **logical update view**
      via a World mutation-generation counter: every clause iteration sees
      the database exactly as of when it started executing — found the hard
      way: append-visibility re-triggered the read-retract-assert idiom,
      and parse-time root snapshots blinded later queries to earlier
      mutations. Clause-DB writes are now mutex-protected (5.1 item pulled
      forward); the versioned-World design of 5.2 remains the eventual
      transactional home. State-machine conformance golden pins it.)*
- [x] File imports / include so programs can be split across files.
      *(2026-08-21: `import "lib.ufy";` — relative to the importing file,
      once-semantics via canonical paths (cycle-safe), inline interleaving,
      located diagnostics for missing files. World is sole FileDebugInfo
      owner via an adopt registry; unify-run passes real filenames so parse
      errors name the file. SPEC.md §15; golden-pinned.)*
- [x] Consistent list/array semantics (construction, unification, member,
      iteration); decide the fate of `include/vault-unify-iterator*-clause.hpp`.
      *(2026-08-21: ArrayTerm is a first-class term kind — `[a, b]` literals,
      element-wise unification with length check, full dispatch/clone/
      traversal integration; the arrays-as-maps quirk is gone. Iteration
      arrives with `foreach` (next item); the legacy `vault::ozw`
      iterator-clause headers are an unrelated external consumer's
      templates and stay untouched.)*
- [x] Restore the example programs (`mediaplayer.ufy` is currently empty) and
      make them part of the golden tests.
      *(2026-08-21: mediaplayer.ufy rewritten as the flagship demo — a
      multi-room audio controller exercising every language feature with a
      24-line golden-pinned narrative. Writing it immediately caught two
      real bugs: the foreach body-local variable threading bug (fixed) and
      concat's `=`-only recognition tripping natural usage (documented).
      With this, every Phase 2 feature item is done: arithmetic, cut,
      findall, arrays, loops/ranges, assert/retract, strings, imports,
      examples.)*

## Phase 3 — Engine robustness and performance

- [ ] First-argument (name/arity) clause indexing: replace the linear
      `ClauseIterator` scan with a `(name, arity) → clause list` index. This
      dominates every other optimization.
- [ ] Atom interning (single table, id-compare instead of string-compare).
- [ ] Bounded slices: `performSlice()` takes a step budget and yields; jobs
      re-queue as READY when the budget is exhausted.
- [ ] Profile on realistic rule sets (`pathfinder.ufy`-scale and larger);
      record baseline numbers in this file.
- [ ] Depth/step limits with a proper error for runaway recursion.

## Phase 4 — Tooling and polish

- [x] REPL (readline is already linked) with query, assert, and inspection.
      *(2026-08-25: `unify-run` with no program -- or with `-i` -- drops into
      an interactive session, `tools/unify-repl.cpp`, driving the same
      RuntimeContext a batch run does, so definitions accumulate across the
      session and `assert`/`retract` need nothing REPL-specific. Query: the
      language's own `query { ... }` plus a `?` shorthand, and every finished
      query reports its variable bindings from `SolveJob::getSolutionList()`
      -- the one thing a batch run cannot show. Inspection: `:list [name]`
      over the root execution state, filtering out builtins and
      loop-desugaring artefacts. Readline is now genuinely optional and
      auto-detected (`UNIFY_USE_READLINE`), with a plain-stdin fallback, so
      CI gains no dependency. One engine-side change was needed to make the
      prompt usable at all: `setDebugTraceEnabled()`, a runtime master switch
      ANDed with the existing compile-time `VAULT_UNIFY_*` categories, which
      the REPL turns off (`--trace` puts it back) -- silencing the trace with
      a shell redirect would have taken parse-error diagnostics with it.
      Batch mode, and therefore every golden test, is byte-for-byte
      unchanged.)*
- [ ] Finish the xdebug debugger front-to-back (breakpoints, stepping,
      variable inspection are scaffolded — make one editor integration work).
- [x] User documentation: tutorial + the Phase 1 spec.
      *(2026-08-21: `LANGUAGE.md` — a ~1000-line tutorial for developers
      with no Prolog background, every example lifted from the
      golden-verified conformance corpus with real outputs, plus a
      reference appendix (reserved words, precedence, builtins,
      limitations with workarounds). Complements the normative SPEC.md.)*
- [ ] Better runtime diagnostics: warnings for unbound-variable printing,
      unknown predicates (typo detection via name/arity index).
      *(2026-09-06: the name/arity index this needs now exists — engine item
      E2's definition catalogue, `World::copyCatalogue()`. The diagnostics
      themselves are still to write.)*

### Engine work pulled in by the lens plan (E1–E16)

`plans/todo/lens/` specifies a text-mode environment whose critical path
runs through the engine rather than the UI. These are its engine items,
recorded here because several are Phase 3/5 work brought forward and should
not be filed under Phase 4 tooling.

- [x] **E1 — Clause provenance.** Origin kind (module / asserted / builtin /
      synthesized / transcript), module id, file and line on every `Clause`,
      stamped at `ExecutionState::appendClause()`.
      *(2026-09-06. Replaces two guesses in `unify-run`'s `:list` — a
      `dynamic_cast<const SimpleBuiltinClause*>` and a `__` head-name prefix
      test. Verified the way the plan's handoff asked: `:list` rewritten to
      use provenance produces byte-identical output to the heuristic
      version, with exactly one intended difference — a user predicate named
      `__cache` is now listed, because it is the user's. Note the first
      attempt derived the origin from the clause's `DebugLocation` and was
      wrong twice over: the parser never sets one on a clause, and every
      builtin sets one pointing at its own C++ source, so builtins were
      being given modules named after `vault-unify-clause-builtin-*.cpp`.
      The engine test caught both.)*
- [x] **E2 — Definition catalogue.** `(name, arity, module) → {clause count,
      retired count, origin, generation}`, maintained incrementally at the
      two points that mutate the database.
      *(2026-09-06. `World::copyCatalogue()` snapshots under
      `clauseDbMutex()`, which is the reader-safety half of E14.
      `retiredCount` is deliberately exposed: clauses are tombstoned and
      never removed, so it is the number that grows without bound under
      repeated redefinition, and gate G3.7 cannot be written against a
      number nobody records.)*
- [x] **E4 — Output redirection.** A per-`Engine` `OutputSink` for
      `print`/`emit`/world-change logging instead of `std::cout`/`std::cerr`.
      *(2026-09-06. The default sink reproduces the previous bytes exactly,
      newline and flush included — `std::endl` is both — so the golden
      corpus is unchanged. Note `emit` has two sinks and always did: the
      text line, now redirectable, and the `UserEventListener` fan-out,
      which is an event bus and stays as it was.)*
- [x] **E10 — Structured diagnostics.** Parse, import and runtime errors as
      a `Diagnostic` value (severity, file, line, column, message, offending
      source line) through a `DiagnosticSink`.
      *(2026-09-06. The default sink prints byte-for-byte what the three
      `fprintf(stderr, ...)` calls in `reportParseError()` printed. One
      asymmetry is deliberate and is spelled out in
      `Engine::DiagnosticDefault`: a parse error always printed, and still
      does; a runtime `UnifyError` never printed anything, and still does
      not, because "unify-run keeps working, unchanged" is a rule of the
      lens plan. An installed sink receives both. `SolveJob` also keeps the
      full list now, not just a count and the last message.)*
- [x] **E14 — The ROADMAP 5.1 concurrency subset.** See 5.1 above.
      *(2026-09-06. Done as specified — atomic counters, locked
      `setTermDebugInfo`, reader-safe catalogue reads — and the plan's own
      warning that "E14's scope may be larger" turned out to be right: see
      E16.)*
- [x] **E16 — Race-free clause-list traversal.** *(Added and closed
      2026-09-06.)* The last thing TSan reported, and the one E14 did not
      cover: `ExecutionState::ClauseIterator` walked
      `std::list<Clause*> m_listClauses` unlocked while `appendClause()`
      pushed onto it. Not introduced by the lens work — the same
      measurement at `39023de` reports it — but the documented "reader side
      deliberately left UNLOCKED" justification ("exactly one reader today,
      the single worker thread") was already false, because
      `parseExecuteSegment()` launches queries mid-parse and keeps parsing.

      Fixed by replacing the list with `ClauseStore`: append-only,
      segmented, with readers taking a snapshot of an atomic count and the
      writer publishing with a release store. No lock on the read path,
      which matters because `isValid()` runs once per candidate clause per
      goal and lookup is already a full linear scan. Segments are allocated
      once and never move, so `ClauseIterator` became a bare index and
      copies of one (`UnifyContext::m_itClause`) are trivially safe.

      Not a behaviour change: snapshotting the count is equivalent to the
      old end-sentinel walk, because a clause appended afterwards has an
      `appendGeneration` above the iterator's `snapshotGen` and was already
      being skipped — the difference is that the slot is no longer READ in
      order to decide to ignore it. `:list` output is byte-identical and
      the golden corpus is unchanged.

      **Result: TSan-clean.** 164 race reports at `39023de`, 13 after E14,
      0 after E16, over `ctest` plus every `.ufy` in the corpus. Measured
      cost: none — a scan-heavy benchmark (400 facts, 601 lookups) runs in
      4.83–4.88 s against 4.87–4.88 s for the `std::list` version.

      Two limits stated rather than left to be found: the directory is
      fixed at 8192 segments of 1024, so one `World` holds 8,388,608
      clauses and `appendClause()` reports and refuses beyond that; and
      contiguous segments are a better substrate for Phase 3's
      first-argument indexing than a linked list was, which is the next
      thing that should touch this structure.

- [x] **LocalSession** — `Session` over the in-process engine
      (`src/vault-unify-local-session.*`), the only place in the tree that
      sees both `SolveJob` and the boundary. *(2026-09-06. Closes gate G0:
      the contract suite passes against it, the fake, and the fake with
      every independent reply reordered. Deliberately asynchronous even
      though it could answer directly, and deliberately without a
      `waitForEngineIdle()` — which is what made E14 and E16 prerequisites.
      Two bugs it surfaced immediately, both pre-existing: a leaked
      `AbstractTermIterator` in `TermTraversable::applyVisitor()` — every
      other call site deletes it, that one did not, and batch runs never
      reach it because only `getSolutionList()` does; and the quiescence
      question "has everything been delivered?", which is not the same as
      "is the queue empty", since an event is popped before its callback
      runs.)*
- [ ] **E7 — Structured solution values.** *(Already listed below as "not
      started"; restated here 2026-09-08 with what writing lens's `--spec`
      runner turned up, and planned in detail in
      `plans/todo/lens/E7-STRUCTURED-VALUES.md`.)*
      `vault-unify-session.hpp` defines a full `Value` (Atom, Int, Float,
      Str, Var, Cons, Array, Map) and documents it as mirroring the
      engine's term kinds one for one. Nothing fills it:
      `SolveJob::getSolutionList()` calls `toString()` on each bound term
      and `LocalSession::onJobFinished()` wraps the resulting text as
      `Value::Kind::Str`, so every binding crosses the boundary as a string
      no matter what it is — which `LocalSession` declares honestly through
      `Capabilities::structuredSolutions == false` and a comment at
      `vault-unify-local-session.cpp:72`. A front end that wants the
      structure has to re-parse the engine's own syntax, which is a second
      parser for one language living in the wrong module: lens's spec files
      are flat facts today for exactly this reason
      (`lens/test/spec/interaction.ufy` says so at length), and one contract
      case is skipped waiting for it. Note the leaves are textual in the
      engine too — an integer literal is an `Atom`-named `ConsTerm` — so
      Int/Float/Str classification is a lexical decision this item has to
      make and document.
- [ ] **A negative literal in a data position loses its sign.** *(Found
      2026-09-08.)* `d( [ x, -1, 2 ] );` is stored as `d( [x, ( 1 ), 2] )`.
      The parser reads the `-` as something other than part of the number
      and silently drops it — silently is the bad part; a term that means
      something else than it says is worse than a parse error.
- [ ] **An empty list literal does not parse.** *(Found 2026-09-08.)*
      `f( [] );` is a parse error, although `findall` produces `[]` as a
      value and prints it. The language can therefore write a value it
      cannot read back, which breaks the round trip an image writer (E6)
      will need.
- [ ] **E3, E5, E6, E9, E11, E12, E13, E15** — not started. (E7 is above,
      with a plan.) E9 (world
      reset) and E12 (staged parse and commit) are the two the lens plan
      calls out as larger than they look; `RuntimeContext` binds one Engine
      and World for life and `~RuntimeContext` deliberately leaks the
      Engine, so `load` needs a real shutdown path first.

---

## Phase 5 — Multithreading / asynchrony outlook

The architecture already points the right way (jobs, slices, blocked queue,
continuation contexts, copy-on-write world comments). The work is making the
sketches real, strictly in this order — memory safety and tests (Phases 0–1)
come before enabling the second worker thread.

### 5.1 Job-level parallelism (first, cheapest real win)

One query = one `SolveJob`; jobs are almost fully self-contained. For N
workers running N independent queries:

- [x] Make all static id counters atomic (`UnifyContext::m_iidLast`,
      `uidNextUnifyContext`, `VarTerm::m_counterUidTerm`, `Clause::m_nextUid`,
      `Job::m_idNextJob`).
      *(2026-09-06, as engine item E14 of the lens plan — pulled forward
      because a session that no longer waits for engine idle makes the
      parser thread and the worker genuinely concurrent. Also caught two
      counters the list above missed: `ClauseContext::m_anonClauseIndex`
      (process-wide, shared by every World, so two parses could hand the
      same `__fe__N` to two different constructs) and xdebug's
      `fakeBreakpointId`. And one outright bug found while inventorying
      them: `Job::getId()` returned `m_idNextJob`, the static counter,
      instead of `m_id` — so every live job reported the same id and every
      "Job %lld ..." trace line in the engine has always been wrong.
      Measured with TSan over the whole corpus: 164 race reports before,
      13 after.)*
- [ ] Lock (or make atomic) the `Job` state machine (`state()`,
      `setDebugTargetState()` — the `TXWTODO: Mutex` items).
- [x] Implement the commented-out locking in `ExecutionState::appendClause` /
      `fork`, or make the clause database append-only + snapshot-read.
      *(2026-09-06, as engine item E16 — the second of the two options, and
      the reader side is what needed it. See E16 below.)*
- [x] Protect or freeze `World`'s debug-info map (`setTermDebugInfo`).
      *(2026-09-06, engine item E14. The two `TXWTODO: Lock begin/end`
      comments now have a real mutex, and `getTermDebugInfo()` takes it too
      — the reader needed it as much as the writer, since a concurrent
      `std::map` insert and lookup is undefined behaviour outright rather
      than a benign word-sized race. Deliberately a SEPARATE lock from
      `clauseDbMutex()`, which `appendClause` already holds when it calls
      in; lock order is clause-db first, debug-info second.)*
- [ ] Implement `removeUserEventListener`; make listener dispatch safe against
      concurrent add/remove.
- [ ] Engine shutdown and job cancellation: `executionLoop` must be able to
      exit; jobs must be stoppable.
- [ ] Fix `vault::BoostAsioIoService` (the vendored `include/vault/vault.hpp`,
      shared with the vault repository): its constructor lets a joinable
      `boost::thread` local go out of scope — the same `std::terminate()`
      bug fixed in `Engine::addWorkerThread()`. Only reached from
      `XDebugTCPClient::connect()` within unify, but every module using the
      singleton is affected under modern Boost.
- [ ] Run the full test suite under TSan with 2+ workers.
      *(2026-09-06: done for ONE worker, which is what the engine creates
      today — `ctest` and the whole `.ufy` corpus are TSan-clean as of E16,
      down from 164 race reports at `39023de`. The "2+ workers" half is
      still open and is not just a matter of calling `addWorkerThread()`
      twice: `executionLoop()` holds `m_mutex` across the whole loop and
      blocks forever when idle, and `waitForEngineIdle()`'s barrier-job
      trick assumes a single FIFO worker.)*

### 5.2 Immutable program, versioned world

- [ ] Make parsed terms and clauses immutable by construction (const-correct
      the term APIs; they nearly are already).
- [ ] Implement the MVCC/copy-on-write `World` described in the header
      comments: each job captures an immutable snapshot (`shared_ptr` to a
      version); `assert`/`retract` produce a new version; old versions die
      with their last job. Lock-free reader parallelism, transactional rule
      updates, no epoch-reclamation machinery needed.

### 5.3 Real asynchrony via continuations

- [ ] Bounded `performSlice` (Phase 3 item) as the prerequisite.
- [ ] Complete the `ClauseContinuationContext` path: an async builtin
      (Z-Wave command, network call, timer) returns *pending*; the job moves
      to `m_lsBlockedJobs`; the completion callback re-readies it. The
      explicit `SolveContext` stack already makes job state heap-resident and
      resumable — this is wiring, not redesign.
- [ ] Add a timer/IO reactor (boost::asio or std-based) the engine owns.
- [ ] Convert one real driver builtin (e.g. `zw_node_value`) to the async
      path as the proof.

### 5.4 Intra-query parallelism (last; only if profiling demands it)

- OR-parallelism (candidate clauses tried concurrently) fits the binding
  model — each candidate already gets its own child `UnifyContext`, similar
  to binding-environment schemes in Aurora/Muse-style parallel Prologs.
- AND-parallelism corresponds to the "parallel goal attribute" mentioned in
  the header comments.
- Both are research-grade complexity (solution ordering, negation, cut
  interaction). Explicitly deferred: 5.1–5.3 deliver the concurrency that the
  event-driven home-automation workload actually needs.

### 5.5 Cross-cutting modernization

- [ ] Migrate `boost::shared_ptr` / `boost::thread` / `boost::function` →
      `std::` equivalents (C++17) while touching the code anyway.
- [ ] CI matrix: ASan + TSan runs; a deterministic single-threaded scheduler
      mode so concurrency bugs are reproducible rather than folklore.

---

## Milestones

| Milestone | Contents | Exit criterion |
|---|---|---|
| M1 "Honest build" | Phase 0 | CI green: builds + golden tests pass |
| M2 "Correct core" | Phase 1 | Spec exists; ASan-clean; structured solutions |
| M3 "PoC language" | Phase 2 | pathfinder/mediaplayer-class programs run end-to-end |
| M4 "Fast enough" | Phase 3 | Indexed lookup; bounded slices; baseline perf recorded |
| M5 "Concurrent engine" | 5.1–5.3 | TSan-clean with N workers; one async builtin in production path |
