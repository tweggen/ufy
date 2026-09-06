# Handoff — start here

For the agent picking this up on the implementation machine.
Written 2026-09-06 by the agent that drafted the plan, on a machine with no
Boost toolchain and therefore **no ability to compile or run anything**.
Everything in these documents is derived from reading the sources at
`a20ddda` and from an independent architecture review; **none of it has been
executed.** Treat every claim as a hypothesis with a file:line citation, not
as a verified fact.

---

## 0. First, do this

```bash
cmake -S unify -B build/unify -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/unify --parallel
ctest --test-dir build/unify --output-on-failure
```

If that is not green on your machine, stop and fix that first — the whole
plan rests on the existing golden suite being the regression net, and every
gate below says "engine goldens stay byte-identical".

Record the baseline: `ctest` output, and the leak numbers from a
`-DUNIFY_SANITIZE=address,undefined` build. You will be diffing against
them for months.

## 1. Read in this order

1. [`../lens-text-mode-environment.md`](../lens-text-mode-environment.md)
   — the plan. §5 (engine work) and §12 (review record) are the two
   sections that matter most; §12 lists what the first draft got wrong, and
   the same mistakes are easy to make again while implementing.
2. [`SESSION-API.md`](SESSION-API.md) — you are building this first.
3. [`ACCEPTANCE.md`](ACCEPTANCE.md) §G0 — the tests you write before the
   code.
4. The rest as you reach them. [`UI.md`](UI.md) and
   [`IMAGE-FORMAT.md`](IMAGE-FORMAT.md) are not needed for weeks.

## 2. The one thing to internalise

**This is an engine project with a UI on top, not a UI project.** The
critical path runs E1 → E2 → E4 → E10 → E14 before a single character is
drawn on a screen. If you find yourself writing FTXUI code in week one,
something has gone wrong.

The corollary: **do not start with the TUI because it is more fun.** A lens
built on an unconverted engine will encode the engine's current
assumptions — synchronous calls, stdout output, idle barriers — and those
assumptions are precisely what the plan exists to remove.

## 3. Your first task, concretely

Gate G0 ([ACCEPTANCE.md](ACCEPTANCE.md)), in this order:

1. **Write `FakeSession` and the contract suite first.** Before
   `vault-unify-session.hpp` has a single implementation. The suite defines
   the interface better than the header does, and writing it first is what
   stops the interface from quietly acquiring in-process assumptions.
2. **`vault-unify-session.hpp`** — the types from
   [SESSION-API.md](SESSION-API.md) §2–4. Add the compile-isolation target
   (G0.1) in the same commit, so it can never regress.
3. **E1 (clause provenance)** — origin + kind on `Clause`, set at
   `ExecutionState::appendClause()`. Smallest of the engine items and
   unblocks E2 and the image work. Verify by making
   `unify-repl.cpp`'s `:list` use provenance instead of its
   `dynamic_cast` + `__`-prefix heuristic (`unify-repl.cpp:489,497`) and
   confirming its output is unchanged.
4. **E2 (catalogue)**, **E4 (output redirection)**, **E10 (structured
   diagnostics)** — in any order.
5. **E14 (the ROADMAP 5.1 subset)** — atomic id counters, locked
   `setTermDebugInfo`, reader-safe catalogue reads. See
   [ARCHITECTURE.md](ARCHITECTURE.md) §3.1 for why this is a G0
   prerequisite and not a later cleanup. **This is the item most likely to
   be deferred and most damaging to defer.**
6. **`LocalSession`** — last, once everything it needs exists.

## 4. Traps, with citations

Each of these was found by reading the source during design. Verify each
one on your machine before relying on it; several are the reason a design
decision looks odd.

| Trap | Where |
| --- | --- |
| `parseExecuteSegment()` **interleaves parse and execute** — appends clauses as it parses and launches a `SolveJob` mid-parse for a `query` block. Staged/atomic parsing is real work (E12), not a refactor. | `vault-unify-runtime-context.cpp:158-222` |
| `ExecutionState::fork()` and `m_listChildStates` are **documented dead code**. Do not build staging on them without reviving them properly. | `vault-unify.hpp:414,444` |
| Diagnostics exist only as **formatted stderr text**; callers get an error *count*. `SolveJob` keeps a count and the *last* message only. E10 is engine work. | `vault-unify-runtime-context.cpp:99-102`; `vault-unify-solvejob.hpp:161,167` |
| `print` emits `"print: " << s` to `std::cout`. The default sink must reproduce that byte-for-byte or the goldens break. | `vault-unify-clause-builtin-print.cpp:28` |
| `RuntimeContext` binds one Engine and World for life; `~RuntimeContext` **deliberately leaks the Engine** (no shutdown path). `load` needs E9. | `vault-unify.hpp:2253` (comment) |
| The clause-DB **reader side is unlocked on purpose**, justified by "exactly one reader today". Dropping `waitForEngineIdle()` invalidates that justification. | `vault-unify.hpp`, `clauseDbMutex()` comment |
| `waitForEngineIdle()` works by queueing a **barrier job** and depends on a single FIFO worker in one address space. | `unify-tool-support.hpp:28-53` |
| Clauses are **tombstoned, never removed**, and lookup is a linear scan. Interactive redefinition degrades the DB monotonically — G3.7 exists to catch this, and it may force ROADMAP Phase 3 indexing earlier than planned. | `vault-unify.hpp:424`; ROADMAP Phase 3 |
| Solutions are `map<string,string>` and their term trees are freed with the job. `inspect` needs E15 (retention). | `vault-unify-solvejob.hpp:92` |
| `for`/`foreach`/`if` desugar at parse time into `__fe__N`/`__feb__N`/`__for__N`/`__if__N`. This is why images store source, not printed clauses. | `vault-unify-parser.cpp:722,1099,1354` |
| The import once-registry keys on `boost::filesystem::canonical()` — **uncomputable on a machine lacking the files**, which is the case images exist for (E13). | `RuntimeContext::m_importedFiles` |
| `.github/` and `.forgejo/` workflows are twins and **must be kept in sync** (README). Note the caveat in G8.1 about runner parity. | repo README |

## 5. Rules the plan will be judged by

- **No production line before a failing test names it.** The gates are
  written so this is possible; if a criterion resists being written as a
  test first, the criterion is wrong — fix it in
  [ACCEPTANCE.md](ACCEPTANCE.md) rather than skipping it.
- **Engine goldens stay byte-identical** at every step. E4 and E10 touch
  output paths; that is exactly when this rule earns its keep.
- **Zero warnings under `-Wall -Wextra`**; sanitizer-clean; zero leaks —
  the bar the engine already meets.
- **`unify-run` keeps working, unchanged.** Batch mode and the existing
  REPL ship as they are; lens is an addition. The CI harness depends on it.
- **Do not ship a control that lies.** `cancel` and `demand` are both
  partly aspirational in v1 (plan §6); the UI says so in the status line.
  Apply the same rule to anything else you find yourself half-implementing.

## 6. If you disagree with the design

Good — you can compile, and the author of these documents could not. The
review in §12 of the plan improved it substantially; a second pass from
someone who can actually run the thing will improve it again.

Specific things worth re-litigating once you have real numbers:

- **FTXUI.** Chosen from documentation, not from use. If it disappoints on
  Windows or its input decoding fights the design, the containment rule
  (G1.5) exists precisely so swapping it is a two-file job. Hand-rolled
  ANSI is the named fallback.
- **The `@changes` log.** It solves four problems at once
  ([IMAGE-FORMAT.md](IMAGE-FORMAT.md) §3), which is either elegant or
  overloaded. If replay ordering turns out to be subtle, say so early.
- **E14's scope.** "The 5.1 subset" is an estimate made without running
  TSan. It may be larger.
- **G3.7's bound.** No baseline exists for how fast redefinition degrades
  the database. Measure it before agreeing to a number.

Amend the documents in place, with a dated revision note, the way the
2026-09-06 review notes do. Do not let the plan and the code drift apart
silently — a stale plan is worse than none.

## 7. Housekeeping

- Move this plan `plans/proposed/` → `plans/todo/` when you start, and
  → `plans/done/` when the gates close. Tick gates in
  [ACCEPTANCE.md](ACCEPTANCE.md) as they pass, the way `unify/ROADMAP.md`
  ticks items — with a dated parenthetical saying what actually happened,
  including what turned out to be harder than expected. That style is the
  most useful thing in this repository; keep it up.
- Add E1–E15 to `unify/ROADMAP.md`. Several (E9, E11, E14) are Phase 3/5
  work this plan pulls forward and should be recorded there as such, not
  filed under Phase 4 tooling.
- `lens/README.md` in the style of `unify/README.md` before G1 closes.
