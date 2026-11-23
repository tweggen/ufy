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

**Gates G0 and G1 are CLOSED. G2 is half open. G3–G9 are untouched.**

Everything is on branch `lens-g0-engine-items`, 32 commits ahead of `main`,
which has never been merged. The branch name stopped being accurate around
the time G1 closed; renaming or merging it is a decision waiting for you.

```bash
# The engine
cmake -S unify -B build/unify -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/unify --parallel
ctest --test-dir build/unify --output-on-failure        # 29/29

# lens, which builds the engine as a subproject
cmake -S lens -B build/lens -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/lens --parallel
ctest --test-dir build/lens --output-on-failure         # 63/63 -- see below

# and the thing itself
./build/lens/unify-lens
```

If either is not green on your machine, stop and fix that first. Every gate
below rests on the suites being the regression net.

The two counts overlap: `lens/CMakeLists.txt` builds the engine as a
subproject, so the 63 in `build/lens` is the engine's 29 **plus** 34 lens
tests (23 golden screens and 11 others). Building lens therefore runs both
suites, which is what the `lens-linux` CI job relies on.

CI runs both, plus ASan/UBSan, plus **a real MSVC build on Windows**
(`.github/workflows/unify-ci.yml`, four jobs, all green). Its `.forgejo`
twin must be kept in sync; the Windows job is deliberately GitHub-only and
both files say why.

### What is done

| | |
| --- | --- |
| **G0 — Boundary** | Closed 2026-09-06. `vault-unify-session.hpp`, `LocalSession`, `FakeSession`, the contract suite: **57 passed, 0 failed, 3 skipped** over three subjects. The three skips are transport-shaped and an in-process session can never satisfy them. |
| **G1 — Shell** | Closed 2026-09-07. Tiling, four stock layouts, 80×24 degradation, `--script`, the command table, generated help, the `F1` Help panel, the `M-x` palette. |
| **G2 — Transcript** | G2.1, G2.2, G2.3, G2.7 done. Define a fact, ask a question, see the binding, against a real engine; parse errors and program output land in the transcript rather than on lens's streams. |
| **Engine items** | E1, E2, E4, E7, E10, E14, E16 done. |

**Engine item E7 (structured solution values) landed 2026-09-08** in seven
phases — see [E7-STRUCTURED-VALUES.md](E7-STRUCTURED-VALUES.md), which is
kept as a worked example of the method: it records what each phase found
that the plan had wrong, which turned out to be the most useful part of it.
Bindings now cross the boundary as trees rather than as strings.

### What is next, in the order I would do it

1. **Finish G2.** Six criteria left and three of them are small. It is the
   panel a user actually uses, and a gate left half-closed is the one most
   likely to rot. `submitTranscriptLine()` was already factored out for
   G2.5, so `F5` re-run is nearly free.
2. **G5 — Inspect.** It was blocked on having term trees to expand and E7
   delivered them, so it just got much cheaper. G5.4's floor is already
   raised.
3. **G3 — Browser**, which needs E3/E5 and is where the catalogue work
   (E2, done) finally shows on screen.

Everything else — G4, G6–G9, engine items E3, E5, E6, E9, E11, E12, E13,
E15 — is untouched. **E9 (world reset) and E12 (staged parse and commit)
are larger than they look**: `RuntimeContext` binds one `Engine` and
`World` for life and `~RuntimeContext` deliberately leaks the `Engine`, so
`load` needs a real shutdown path before either can start.

### Loose ends, none of them blocking

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
| `lens-screen-*` (23) | Golden screens. Layout and degradation only — deliberately *not* the oracle for behaviour, so a cosmetic change does not re-record a dozen files. |
| `lens-interaction` (8) | What happens *between* frames, including a seeded random walk. Written after a user reported that pressing Up after Down did nothing the first time — a bug no single-frame assertion can see. |
| `lens-pty-interaction` (3) | The real binary on a real pseudo-terminal. The only thing that exercises `src/term/`. Written after two bugs lived there unseen. POSIX only. |
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
