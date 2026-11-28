# Handoff — start here

For the agent picking this up next.

**Revised 2026-09-10.** The original version of this file was written
2026-09-06 by the agent that drafted the plan, on a machine with no Boost
toolchain and therefore no ability to compile or run anything; it opened by
saying "none of it has been executed". That is no longer true, and leaving
it would have sent the next reader down a path that is already built. What
follows is the state of the work. §5's history note keeps the original
caveat, because it still governs the parts nobody has reached yet.

---

## 0. Where this actually stands

**Gates G0, G1, G2 and G5 are CLOSED. G3 is half done; G4 and G6–G9 are
untouched.**

*(Revised 2026-11-28.)* PR #1 (G0's engine items), PR #2 (the menu bar)
and PR #3 (the rest of G2) are merged into `main`. G5 is PR #4
(`lens-g5-inspect`); the read-only half of G3 is on `lens-g3-browser`,
built on top of it.

```bash
# The engine
cmake -S unify -B build/unify -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/unify --parallel
ctest --test-dir build/unify --output-on-failure        # 29/29

# lens, which builds the engine as a subproject
cmake -S lens -B build/lens -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/lens --parallel
ctest --test-dir build/lens --output-on-failure         # 72/72 -- see below

# and the thing itself
./build/lens/unify-lens
```

If either is not green on your machine, stop and fix that first. Every gate
below rests on the suites being the regression net.

The two counts overlap: `lens/CMakeLists.txt` builds the engine as a
subproject, so `build/lens` runs the engine's suite **plus** lens's own (25
golden screens and the rest). Building lens therefore runs both suites,
which is what the `lens-linux` CI job relies on.

The G2.8 cases in `lens-pty-interaction` need `unify-run` built with GNU
readline, since without it `unify-run -i` keeps no history file. Where the
headers are missing they **skip and say so**. CI installs `libreadline-dev`
for this reason.

CI runs both, plus ASan/UBSan, plus **a real MSVC build on Windows**
(`.github/workflows/unify-ci.yml`, four jobs, all green). Its `.forgejo`
twin must be kept in sync; the Windows job is deliberately GitHub-only and
both files say why.

### What is done

| | |
| --- | --- |
| **G0 — Boundary** | Closed 2026-09-06. `vault-unify-session.hpp`, `LocalSession`, `FakeSession`, the contract suite: **57 passed, 0 failed, 3 skipped** over three subjects. The three skips are transport-shaped and an in-process session can never satisfy them. |
| **G1 — Shell** | Closed 2026-09-07. Tiling, four stock layouts, 80×24 degradation, `--script`, the command table, generated help, the `F1` Help panel, the `M-x` palette. |
| **G5 — Inspect** | Closed 2026-11-28. The Solutions table, with Down past the last row asking for the next batch (once, however long the key is held); the Inspector, the selected solution as a tree whose cut nodes are fetched with `inspect` and never by solving again; Diagnostics, whose Enter sends the Source panel to the reported line. lens now retains only the query it shows and releases the rest. |
| **G2 — Transcript** | Closed 2026-11-27. Define, query, see the binding; diagnostics and program output in the transcript; one batch of 10,000 solutions at a time; `C-c` cancel that detaches and says the engine cannot stop; `F5` re-run of a past line; `$HOME/.unify_history` shared with `unify-run -i` both ways; `transcript.write-file` through a minibuffer prompt; `F1` on a builtin opening help extracted from SPEC.md and LANGUAGE.md at build time. |
| **Engine items** | E1, E2, E4, E7, E10, E14, E16 done. |

**Engine item E7 (structured solution values) landed 2026-09-08** in seven
phases — see [E7-STRUCTURED-VALUES.md](E7-STRUCTURED-VALUES.md), which is
kept as a worked example of the method: it records what each phase found
that the plan had wrong, which turned out to be the most useful part of it.
Bindings now cross the boundary as trees rather than as strings.

### What is next, in the order I would do it

1. **The rest of G3.** Done: the Catalogue (G3.1), `source` into the
   Source panel (G3.2), refresh on `WorldChanged` (G3.4) and the filter's
   frame budget (G3.5). Open, in the order I would take them:
   - **E3** (predicate-scoped retract), then **G3.3** (edit and `F3`
     under `ReplacePredicates`) and H's policy selector.
   - **G3.7**: measure first. No baseline exists for redefinition growth.
   - **G3.8** (world undo), which needs G3.3.
   - **G3.6**, the editor, the largest single item in the plan
     (UI.md §3.1).

Everything else — G4, G6–G9, engine items E3, E5, E6, E9, E11, E12, E13,
E15 — is untouched. **E9 (world reset) and E12 (staged parse and commit)
are larger than they look**: `RuntimeContext` binds one `Engine` and
`World` for life and `~RuntimeContext` deliberately leaks the `Engine`, so
`load` needs a real shutdown path before either can start.

### Loose ends, none of them blocking

- **E15 is listed as not started, but its effect is already in place.**
  `LocalSession` reports `retention: true`, keeps every solution's value
  (E7's clones outlive the job), and `inspect` works against it — which is
  what G5.2 runs on. `unify/ROADMAP.md` and SESSION-API.md still describe
  E15 as the item that makes this possible. Someone should decide whether
  E15 is done, or whether it names something more (bounded retention,
  say) and rewrite it to say what.
- **The Source panel shows one line.** G5.3 sends it to a diagnostic's
  file, line and column, and all a `Diagnostic` carries is that one line;
  the panel says so. Whole definitions are G3's `source` request.
- **Only lens's own model test can see the one-demand guard.** The
  `lens-session` rig settles the engine after every key, so a demand is
  always answered before the next key arrives. The real-engine case pins
  the path end to end instead, and says why in a comment.

- **A goal that does not parse is reported against the engine's wrapper**,
  `<session>:1:1: parse error` over `query {`, not against what the user
  typed. It has been that way since G2.2; recording G2.9's golden made it
  visible. `LocalSession` wraps the goal before parsing (`wrapGoal`), so
  the column is the wrapper's too.
- **The builtin help is written for implementers.** F1 on `print` shows
  SPEC.md section 9 as it stands: class names, source paths. Accurate, and
  extracted so it stays accurate, but a user-facing paragraph per builtin
  belongs in LANGUAGE.md and the extractor would pick it up.
- **Fixed 2026-11-28: the `-Wdangling-reference` warning was a real bug.**
  `UT_CHECK_EQ` bound `actual` to a `const auto&`, which kept alive only the
  final reference, not the temporary it pointed into. Two menu-bar tests
  (`menuCategories()[ i ]`) read a freed string: it passed under glibc by
  luck and failed under MSVC's debug heap, which made `windows-msvc` red on
  `main` from PR #2 on. The macro now compares inside one function call, and
  the warning is gone. Lesson: a dangling-reference warning in a test
  harness is never "harmless", whatever the reasoning sounds like.

- **Two parser defects, recorded in `unify/ROADMAP.md`, unfixed.** A
  negative literal in a data position **silently** loses its sign
  (`d([-1])` is stored as `d([1])`), and an empty list literal does not
  parse (`f([])` is a parse error) although `findall` produces `[]` as a
  value — so the language can write a value it cannot read back. Both are
  designed around in `lens/test/spec/interaction.ufy`; the silent one is
  the sort that bites someone months later.
- `lens/src/term/ftxui-terminal.cpp` uses `_isatty` on Windows.
  `GetConsoleMode` is the right test under mintty.
- `SolveJob::getSolutionList()` (the string form) has one caller left,
  `unify-repl.cpp:384`, and now diverges from the structured form on
  unbound variables. Documented and pinned by a test, but it is a decision
  someone should take rather than inherit.
- The Windows CI job's engine-golden step is **informational and has been
  passing**; it can be promoted to gating.
- The `origin` remote URL embeds a personal access token in plaintext in
  `.git/config`. Rotate it and use SSH or a credential helper.

## 1. Read in this order

1. **This file.**
2. [`ACCEPTANCE.md`](ACCEPTANCE.md) — the gates, with dated notes saying
   what actually happened. Its "Rules of the method" section is the ten rules
   the work is judged by; rules 8, 9 and 10 were added *after* real users
   reported bugs and are the ones that keep earning their keep.
3. [`SESSION-API.md`](SESSION-API.md) — the boundary. Built; read it when
   you touch anything that crosses it.
4. [`ARCHITECTURE.md`](ARCHITECTURE.md) — the layering rule, enforced by
   `lens/test/check-layering.sh`, not by convention.
5. [`../lens-text-mode-environment.md`](../lens-text-mode-environment.md)
   — the original plan. §12 (review record) is still worth reading: it
   lists what the first draft got wrong, and the same mistakes are easy to
   make again.
6. [`UI.md`](UI.md) and [`IMAGE-FORMAT.md`](IMAGE-FORMAT.md) when you reach
   G3 and G4.

## 2. The test suites, and which one your change belongs in

There are four categories, and the reason there are four is that each of
the last three was added *after* a bug got past the ones before it.
[ACCEPTANCE.md](ACCEPTANCE.md) rules 6 and 8–10 tell the story.

| Suite | What it is for |
| --- | --- |
| `unify-golden-*` (23) | The engine's stdout, byte for byte. **Must stay byte-identical** through everything below. |
| `unify-session-*` (3) | The boundary: compile isolation, header hygiene, and the contract suite over three subjects. |
| `unify-engine-*` (3) | Engine items asserted *at* the engine, where they are implemented, so a regression names the engine rather than the adapter. |
| `lens-layout`, `lens-modreg`, `lens-grid`, `lens-shell` (78 cases) | Model tests. Assert on the model, never on pixels. |
| `lens-screen-*` (26) | Golden screens. Layout and degradation only — deliberately *not* the oracle for behaviour, so a cosmetic change does not re-record a dozen files. |
| `lens-interaction` (10) | What happens *between* frames, including a seeded random walk. Written after a user reported that pressing Up after Down did nothing the first time — a bug no single-frame assertion can see. |
| `lens-browser` (14) | Gate G3 on the model: the catalogue's grouping by provenance, its stubs and pages, `source` for exactly the selected key, refresh on `WorldChanged` with coalescing and no polling, and the filter's 50 ms frame budget over 5,000 keys (optimised builds only). |
| `lens-inspect` (17) | Gate G5 on the model: the Solutions table and its one demand, the Inspector's tree, cut marks and `inspect` grafting, Diagnostics to Source, and their help. Hand-built session events, because the interesting truncations are easiest to state as the tree they are. |
| `lens-session` (13) | A Model and a SessionBridge over a **real** LocalSession, driven the way `--script` drives them and asserted on the model. For claims about the pair that no engine-free test can make: 10,000 solutions, cancel, that every builtin `describe()` reports has a help topic, an 11-deep term opened to the bottom through `inspect`, that the core holds at most one retained query, and that each definition refreshes the catalogue while walking it asks for nothing. |
| `lens-pty-interaction` (5) | The real binary on a real pseudo-terminal. The only thing that exercises `src/term/`. Written after two bugs lived there unseen. POSIX only. Two cases drive `unify-run -i` too, for the shared history file; they skip without readline. |
| `lens-history`, `lens-transcript-write` | The shipped binary and a file on disk: a file is not on the screen, so it cannot be a golden screen. |
| `lens-help-extract` (6 fixtures) | That the build-time help extractor still REFUSES a document it cannot read. The same discipline as `lens-spec-runner`. |
| `lens-spec` (9 cases) | Interaction cases **written in Unify** and run by the shipped binary (`unify-lens --spec`). |
| `lens-spec-runner` (13 fixtures) | That `--spec` still *fails* when it should. A runner that quietly passes everything is worse than none: it looks like coverage. |

## 3. Rules the work is judged by

Unchanged from the original handoff, and all of them have now been tested
in anger:

- **No production line before a failing test names it.** If a criterion
  resists being written as a test first, the criterion is wrong — fix it in
  [ACCEPTANCE.md](ACCEPTANCE.md) rather than skipping it.
- **Engine goldens stay byte-identical** at every step.
- **Zero warnings under `-Wall -Wextra`**; sanitizer-clean; zero leaks.
- **`unify-run` keeps working, unchanged.** Batch mode and the existing
  REPL ship as they are; lens is an addition.
- **Do not ship a control that lies.** `cancel` and `demand` are still
  partly aspirational; the UI says so in the status line. Apply the same
  rule to anything else you find yourself half-implementing.

One rule the work added, worth stating explicitly because it caught three
real defects: **prove the test can fail.** Every phase of E7 reverted its
own fix and recorded the failing output before committing. Two of the bugs
found that way — a use-after-free the obvious test could not have caught,
and an assertion that had never once executed — were invisible to a green
suite.

## 4. Traps, with citations

Each was found by reading the source. The resolved ones are kept because
the reasoning behind a design decision is still the reasoning.

| Trap | Where | State |
| --- | --- | --- |
| `parseExecuteSegment()` **interleaves parse and execute** — appends clauses as it parses and launches a `SolveJob` mid-parse for a `query` block. Staged parsing is real work (E12), not a refactor. | `vault-unify-runtime-context.cpp:158-222` | live |
| `ExecutionState::fork()` and `m_listChildStates` are **documented dead code**. Do not build staging on them without reviving them properly. | `vault-unify.hpp:414,444` | live |
| `RuntimeContext` binds one Engine and World for life; `~RuntimeContext` **deliberately leaks the Engine**. `load` needs E9. | `vault-unify.hpp:2253` | live |
| The clause-DB **reader side is unlocked on purpose**, justified by "exactly one reader today". | `clauseDbMutex()` comment | live, but see E16 |
| `waitForEngineIdle()` queues a **barrier job** and depends on a single FIFO worker in one address space. | `unify-tool-support.hpp:28-53` | live |
| Clauses are **tombstoned, never removed**; lookup is a linear scan. G3.7 exists to catch the degradation and may force Phase 3 indexing earlier than planned. | `vault-unify.hpp:424` | live |
| `for`/`foreach`/`if` desugar at parse time into `__fe__N`/`__for__N`/`__if__N`. This is why images store source, not printed clauses. | `vault-unify-parser.cpp:722,1099,1354` | live |
| The import once-registry keys on `boost::filesystem::canonical()` — **uncomputable on a machine lacking the files**, which is the case images exist for (E13). | `RuntimeContext::m_importedFiles` | live |
| **The engine has no types at term level.** `1`, `red` and `"red"` all parse to the same 0-arity `ConsTerm`. Anything that needs Int-vs-Atom must *decide* it, not recover it. | `vault-unify-parser.hpp:690`; `SPEC.md:50,70` | live, and now load-bearing — see E7 §2 |
| `.github/` and `.forgejo/` workflows are twins and **must be kept in sync**. | repo README | live |
| `print` emits `"print: " << s` to `std::cout`; a sink must reproduce it byte for byte. | `vault-unify-clause-builtin-print.cpp:28` | **resolved** by E4, and the goldens prove it |
| Diagnostics exist only as formatted stderr text; callers get an error *count*. | `vault-unify-solvejob.hpp:161` | **resolved** by E10 |
| Solutions are `map<string,string>` and their term trees are freed with the job. | `vault-unify-solvejob.hpp:92` | **resolved** by E7 — `getGroundedSolutions()` hands out clones that outlive the arena |

## 5. History, and what it still governs

The original handoff was written by an agent that could not compile. Its
claims were hypotheses with citations. Most have since been executed and
either confirmed or corrected — and the corrections are recorded next to
the claims, in [ACCEPTANCE.md](ACCEPTANCE.md)'s dated notes and in
[E7-STRUCTURED-VALUES.md](E7-STRUCTURED-VALUES.md)'s per-phase findings.

**The caveat still applies to everything nobody has reached.** G3–G9 and
engine items E3, E5, E6, E9, E11, E12, E13 and E15 are still described by
documents written from reading. Treat those claims the way the first
handoff asked: as hypotheses with a `file:line`, not as facts.

Two design questions the original flagged for re-litigation, now answered:

- **FTXUI** was chosen from documentation rather than use. It has now been
  used, on Linux, macOS and Windows. It works, and the containment rule
  (G1.5) earned its keep twice: both terminal-layer bugs were fixed in one
  file. Keep it.
- **E14's scope** was "an estimate made without running TSan". The estimate
  was too small: TSan reported 164 races, E14 took it to 13, and the
  remaining 13 needed E16 (a race-free clause store) to reach zero.

Still open for re-litigation: **G3.7's bound** — no baseline exists for how
fast interactive redefinition degrades the clause database. Measure it
before agreeing to a number.

## 6. Housekeeping

- Tick gates in [ACCEPTANCE.md](ACCEPTANCE.md) as they pass, **with a dated
  parenthetical saying what actually happened, including what turned out to
  be harder than expected.** That style is the most useful thing in this
  repository. Every note added since 2026-09-06 follows it; keep it up.
- Amend these documents in place with a dated revision note when you find
  them wrong. A stale plan is worse than none — this file is the proof.
- Move the plan `plans/todo/` → `plans/done/` when the gates close.
- `unify/ROADMAP.md` carries E1–E16 and the two parser defects. Add
  anything new there rather than only in a commit message.
