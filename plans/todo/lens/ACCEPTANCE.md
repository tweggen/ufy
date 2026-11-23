# Acceptance gates

Companion to [`../lens-text-mode-environment.md`](../lens-text-mode-environment.md).
Plan requirement (e): development is test-driven, and progress is measured
by gates whose criteria are executable.

> ## Where the gates stand — 2026-09-10
>
> | Gate | | |
> | --- | --- | --- |
> | **G0** Boundary | **CLOSED** 2026-09-06 | contract suite 57 passed / 0 failed / 3 skipped, over three subjects |
> | **G1** Shell | **CLOSED** 2026-09-07 | tiling, four layouts, degradation, `--script`, commands, help, palette |
> | **G2** Transcript | **PARTIAL** | G2.1, G2.2, G2.3, G2.7 done; G2.4, G2.5, G2.6, G2.8, G2.9 and H open |
> | **G3–G9** | not started | G5 (Inspect) is now much cheaper — it was blocked on term trees and engine item E7 delivered them |
>
> Engine items **E1, E2, E4, E7, E10, E14, E16** are done; **E3, E5, E6,
> E9, E11, E12, E13, E15** are not. `ctest` is 29/29 on `build/unify` and
> 63/63 on `build/lens`; CI additionally builds and tests on Windows/MSVC.
> [HANDOFF.md](HANDOFF.md) §0 is the fuller picture and the place to start.

> **Progress note (2026-09-06, implementation machine). G0 IS CLOSED.**
> The boundary exists, the contract suite runs against three subjects --
> `FakeSession` benign, `FakeSession` reordering, and `LocalSession` over a
> real in-process engine -- and every criterion below is discharged. 56
> passed, 0 failed, 4 skipped. The baseline the gates are measured against
> is in [BASELINE.md](BASELINE.md).
>
> The four skips are all `LocalSession`, all honest, and all printed with
> their reason on every run: three ask for something an in-process session
> has no way to do (fail a transport, drop a connection, deliver on a
> controlled clock). *(2026-09-08: there were four. The fourth needed nested
> values, which engine item E7 has now made possible -- the truncation case
> runs against the real engine and passes, and the retention case takes its
> structured branch rather than its flat one. Three left, all transport-
> shaped.)* None of them is skipped for the fake, so no criterion goes
> unexercised.
>
> Two deviations from the gates as written, both deliberate.
>
> **G0.4's adversity is split.** Reordering and delay are applied to the
> WHOLE suite -- they must never change an outcome, so every case must pass
> identically under both, and a case that secretly depends on reply order
> fails in the reordered run only. Injected failure and disconnection are
> DEDICATED cases instead, because they change outcomes by definition: a
> suite running under them would have to weaken every assertion to "either
> the right answer or a Failed", which is not an assertion. The fake
> reorders logically rather than by sleeping, from a seeded generator, so a
> failing interleaving is reproducible from the seed; one case uses real
> wall-clock delay, the only obligation that is about time rather than
> order.
>
> **Exact counts became invariants where a real core cannot be scripted.**
> G0.7's bad-define case originally asserted "exactly 2 diagnostics", which
> is a property of the fake -- a real parser produces what it produces. It
> now asserts the contract instead: at least one diagnostic, a `Defined`
> alongside it, and `errorCount` equal to the number of diagnostics emitted.
> That is stronger, not weaker: it is the assertion that catches a core
> reporting a boolean dressed up as a count.

> **Revision note (2026-09-06, after architecture review).** Ten gates, not
> nine (they were always G0–G9; the count was simply wrong). Three
> composition gaps the first draft left open are now gated: edit-then-save
> (G4.4 — the case that exposed the image format's central contradiction),
> request-during-query (G0.9 — the concurrency obligation of
> [ARCHITECTURE.md](ARCHITECTURE.md) §3.1), and database growth under
> repeated redefinition (G3.7). Rule 6 is new and demotes character-grid
> goldens from primary oracle to layout check.

---

## Rules of the method

1. **No production line before a failing test names it.** Every criterion
   below is written so it can be a test before it is a feature.
2. **A gate is closed or open — never "mostly".** A criterion that cannot
   be run is not a criterion; rewrite it or drop it.
3. **The existing engine goldens stay green throughout.** Engine items
   E1–E6 touch shared code (clause append, builtins, parser); the `.ufy`
   golden corpus is the regression net and it must be byte-identical at
   every gate, not just at the end.
4. **Every gate carries a help criterion.** Help is built in from the first
   gate ([UI.md](UI.md) §5), so no gate closes with features that `F1`
   cannot explain. This is listed per gate below as **H**.
5. **Every gate carries a warnings criterion.** Zero warnings under
   `-Wall -Wextra`, matching the engine's existing bar, and clean under
   `UNIFY_SANITIZE=address,undefined`.
6. **Assert on the model; use grids for layout.** Character-grid goldens
   embed content — generation counters, clause counts, solution ordering —
   so making them the primary behavioural oracle turns every cosmetic
   change into a dozen re-recordings. Behaviour is asserted on the model
   (`fold` emitted these requests; the model holds these rows); grids are
   reserved for layout, 80×24 degradation, and "this panel renders".
   [ARCHITECTURE.md](ARCHITECTURE.md) §4.
7. **A gate criterion that cannot fail is deleted.** "Help exists" is
   testable; "help cannot drift" is not, and is not claimed.
8. **Assert on sequences, not only on states.** *(Added 2026-09-07, after
   the first real user.)* Rules 1–7 produce two kinds of test: model tests,
   which assert one state, and golden screens, which assert one final frame.
   Every defect a user has actually reported in lens lived in neither —
   they lived in the TRANSITION between frames. "Pressing Up after Down does
   nothing the first time" leaves a final screen that is perfectly
   self-consistent and would pass any golden; what was wrong was that the
   highlight did not move when the key was pressed.

   So a third category: **interaction tests** (`lens/test/interaction-test.cpp`).
   They still assert on the model rather than on pixels, so rule 6 stands
   — they add the dimension neither other category has. Two obligations
   come with them:

   - **Invariants are written once and applied to every panel.** Three
     copies of the same scroll arithmetic is how the bug happened; one
     shared rule with one set of invariants is how it stops happening, for
     the panels that do not exist yet as much as for the three that do.
   - **A seeded random walk.** The layout suite's property test found a
     real bug no hand-written case would have; the same technique pointed
     at interaction immediately found a second one — splitting a tile
     while a help page was scrolled left the cursor off screen, because
     only cursor movement re-followed the cursor and geometry changes did
     not.

9. **The seam the tests cannot cross is where the next bug lives.**
   *(Added 2026-09-08, after the second and third real user reports.)* Rule
   8's interaction tests drive the model directly, which is what makes a
   thousand key sequences cost milliseconds — and it means `src/term/` was
   the one file nothing exercised. Two user-visible bugs then turned up
   inside exactly that gap: a frame computed and never presented (FTXUI
   skips a redraw when none of its own events fired, and lens changes the
   grid out of band), and Ctrl-C killing the process instead of completing
   the `C-x C-c` chord. Neither was reachable from any test above.

   So a fourth category, deliberately tiny: **terminal tests**
   (`lens/test/pty-interaction-test.cpp`) run the real binary on a real
   pseudo-terminal and read the bytes back. Three cases, seconds to run.
   The rule that keeps it tiny is that it asserts only what no other
   category *can* — that a keystroke produces a frame, and that quitting
   quits — and never behaviour, which belongs one layer down where it is
   cheap. POSIX only; the bug class is not platform-specific, so catching
   it on one platform catches it.

10. **The interaction cases are written in Unify.** *(Added 2026-09-08.)*
    lens is the front end for a logic engine and ships one linked in, so
    the cases that say what a keystroke must do — sequence plus
    expectations, which is data — are stated in the language the product
    exists to run and executed by the shipped binary (`--spec`,
    `lens/test/spec/interaction.ufy`). This is dogfooding that pays twice:
    a user can send a failing case as a file rather than a description, and
    the engine's own gaps become visible to the people who can fix them
    (writing the runner turned up three, now recorded as ROADMAP items —
    the unfilled structured `Value`, a negative literal that loses its
    sign, and an empty list literal that will not parse).

    Two obligations:

    - **The vocabulary is closed.** An expectation the runner does not
      recognise fails the case. A spec language that ignored what it did
      not understand would turn every typo green, which is the one outcome
      a suite must never have — as would a stray case id whose facts are
      silently dropped.
    - **The runner is itself tested for failing.** `test/spec/bad/` holds
      one fixture per way a spec can be wrong and `check-spec-runner.sh`
      asserts each is still caught. A runner that quietly passes everything
      is worse than no runner: it looks like coverage.

Three harnesses, all in the repo's established golden style
(`test/run-golden-test.sh`, regenerate with `UNIFY_UPDATE_GOLDEN=1`):
**contract suite** (D7, parameterised over `Session` implementations),
**golden screens** (D8, key-script + geometry → character grid), and the
**engine goldens** (existing).

---

## G0 — Boundary

*The abstraction exists and is provably not leaky.*

| # | Criterion |
| --- | --- |
| G0.1 **[done]** | `vault-unify-session.hpp` compiles against a translation unit that includes **no other unify header**. Checked by a dedicated compile target, so a stray `Clause*` cannot appear later. |
| G0.2 **[done]** | `grep` over the public session header finds no `Clause`, `UnifyContext`, `World`, `SolveJob`, `Engine`, `boost::`, or any raw pointer type in a signature. Automated. |
| G0.3 **[done]** | The full contract suite ([SESSION-API.md](SESSION-API.md) §7) passes against `LocalSession`. |
| G0.4 **[done, with the deviation noted above]** | The same suite, unmodified, passes against `FakeSession` — which reorders independent replies, delays them up to 500 ms, injects `Failed`, and disconnects mid-query. |
| G0.5 **[done]** | Every test observes strictly increasing, gapless `seq`, and never a `Solution` after its query's terminal `QueryStatus`. |
| G0.6 **[done]** | `solve(goal, initialDemand=3)` on a 10-solution goal delivers exactly 3, then stops until `demand`. |
| G0.7 **[done]** | Engine items E1 (provenance), E2 (catalogue), E4 (output redirection), E10 (structured diagnostics) are in place: `listing` returns correct kinds with no `__` name-prefix heuristic; `print` arrives as an `Output` event with nothing on the process's stdout; a parse error arrives as a `Diagnostic` with structured file/line/column, not as text on stderr. |
| G0.8 **[done]** | **Query attribution.** Two queries run concurrently against `FakeSession`; every `Output` and `Diagnostic` they cause carries the right `QueryId`, and each query's `querySeq` is gapless. |
| G0.9 **[done]** | **Request during query.** `define`, `listing` and `source` issued while a query is running are answered in bounded time and the catalogue stays correct — the obligation of [ARCHITECTURE.md](ARCHITECTURE.md) §3.1. Engine item E14 (the ROADMAP 5.1 subset) is in place; the suite runs under TSan for this case. *(2026-09-06: the behavioural half passes against the fake. E14 is done and took TSan reports over the corpus from 164 to 13 — but the residual 13 are the unlocked clause-list reader racing `appendClause`'s `push_back`, which E14 as written did not cover. It is pre-existing, not introduced here: the same measurement at `39023de` reports 164. Recorded as engine item E16 and now FIXED: the clause list is an append-only segmented store with a snapshot-count read, at no measurable cost. The criterion's own case now runs against a real LocalSession -- a query is left mid-flight with 1 of 50 solutions delivered, `define`, `listing` and `source` are all answered while it is unfinished, and the query then resumes intact. The whole suite is TSan-clean, as is the .ufy corpus.)* |
| G0.10 **[done]** | **Query lifetime.** After a terminal `QueryStatus`, `inspect` works while `retained`; `release` frees it; a subsequent `inspect` answers `Failed`; the core's retained-query count returns to zero. A suite that runs 1,000 queries and releases them ends with the same retained count it started with. |
| G0.11 **[done]** | **Debug is closed over its events.** `debug(SetTrace)` produces `TraceEvent`s; `demand(q, Stream::Trace, n)` bounds them; a core reporting `debug: false` answers `Failed` and never crashes. |
| **H** | `describe()` reports the core's builtin list; the help extraction step (UI §5.3) consumes it. |

## G1 — Shell

*A window manager and a help system, before any feature needs either.*

> **Progress note (2026-09-07). G1 IS CLOSED.** All seven numbered criteria
> and the "H" criterion pass. FTXUI is in, pinned to v7.0.3, in one file
> behind `ITerminal`. Seventeen golden screens, all recorded through
> `--script` with no terminal involved.
>
> One limitation stated rather than discovered later: the FTXUI backend is
> compile- and link-verified but its INTERACTIVE behaviour is not tested,
> because the machine this was written on has no tty. Everything a golden
> can prove is proved through `--script` with no terminal at all, which is
> why that mechanism was built first; what remains untested is one file,
> deliberately kept as small as it is so that "we could not test it" covers
> as little as possible. **A first run on a real terminal is owed.**

| # | Criterion |
| --- | --- |
| G1.1 **[done]** | Golden screens for all four stock layouts at **120×40** and **80×24**. *(2026-09-07: eight screens, recorded through `--script` so no terminal is involved. The four layouts are stances rather than box arrangements -- browse is the system-browser stance, run drives a program, debug asks why, full is for the person with 200 columns -- and none special-cases the small geometry: at 80×24 each degrades through the ordinary solver, so there is no second set of presets to keep in step.)* |
| G1.2 **[done]** | A key script that splits, cycles focus, closes and maximises tiles reproduces its golden exactly; the tile solver always covers the tile area with no gap and no overlap (asserted structurally, not just visually). *(2026-09-07: three key-script goldens, and the structural half is a seeded property test walking every cell of the area over forty random layouts at five geometries. `checkCoverage()` is exported so the same invariant can be asserted on any frame, not only in the layout tests.)* |
| G1.3 **[done]** | Resize from 120×40 to 80×24 and back restores the **pre-degradation layout tree**, not merely a valid one: the solver keeps demoted tiles' geometry rather than discarding it, and the criterion asserts tree equality. (Stated this way because the naive reading — "the model is identical" — is false for a lossy solver and would have been quietly weakened later.) *(2026-09-07: made true by construction rather than by care -- `solve()` is a pure function and never touches the tree, so degradation is a property of one call. Asserted twice: tree equality in the model tests, and on screen as an equivalence between two runs, since what a user notices is that the round trip leaves EXACTLY the screen they would have had without resizing.)* |
| G1.4 **[done]** | Below 80×24, lens exits with the pinned message and a non-zero status; it does not render. *(2026-09-07: all three halves asserted separately, because each fails differently -- a zero exit makes a script think it worked, a missing message leaves the user with no idea why, and rendering anyway produces the illegible screen the rule exists to prevent. The test also asserts 80×24 itself WORKS, so the gate is not an off-by-one.)* |
| G1.5 **[done]** | FTXUI containment: no file outside `lens/src/term/` includes an FTXUI header. Automated grep, gating. *(2026-09-07: FTXUI v7.0.3, pinned, in one file behind `ITerminal`. Verified the grep can FAIL on a deliberately bad tree, not merely pass on this one.)* |
| G1.6 **[done]** | `model/` and `panels/` include no engine header. Automated grep, gating. *(2026-09-07: with one deliberate exemption -- `vault-unify-session.hpp` IS permitted, since it is the boundary, G0.1 proves it pure, and seeing it is the point of having drawn one. `layout/` and `modreg/` are held to the stricter rule of no dependencies at all.)* |
| G1.7 **[done]** | `--script FILE` runs headless, dumps the final grid and exits — the mechanism every later golden depends on. *(2026-09-07: one step per line, spelled as the keymap spells it. `resize COLSxROWS` is a step too -- G1.3's claim is about what a resize does to a layout, and a script that could only type keys could not express it.)* |
| **H** **[done]** | `F1` opens the Help panel from every focused region; the hint line is non-empty in every golden; `M-x` lists every registered command; the generated keymap page matches the live command table (asserted, not eyeballed); a command registered without help text fails the build. *(2026-09-07: `F1` opens help BESIDE the work rather than over it -- the whole argument for tiling -- reuses its tile rather than filling the screen, and is contextual (from inside help it opens the help topic). `M-x` is a tile, not an overlay, so it obeys the same solver and cannot clip at 80×24; it lists every command including unbound ones, since discovering a command exists must not require it to have a key, and it is modal, so typing `x` filters rather than beginning a `C-x` chord. The command table refuses blank help at registration. Two bugs the tests caught immediately: the help contained the literal text `[[link]]` as prose, which parses as a link to a topic that does not exist -- a dead link in the first help page anyone reads -- and the palette had to be closed BEFORE running a command, or a layout command would split the palette's own tile instead of the user's.)* |

## G2 — Transcript

*The REPL, better.*

> **Progress note (2026-09-07).** G2.1, G2.2, G2.3 and G2.7 are done, against
> a real in-process engine through `LocalSession`. The transcript is a panel
> like any other; the model sees `vault-unify-session.hpp` and nothing else
> of the engine, so it still needs no engine to be tested.
>
> Outstanding: G2.4 (responsiveness under 10,000 solutions), G2.5 (`F5`
> re-run in place -- `submitTranscriptLine()` is already factored out so the
> re-run issues the identical request rather than a reconstruction), G2.6
> (cancel), G2.8 (history file interop with `unify-run -i`), G2.9
> (`transcript.write-file`), and the "H" criterion.
>
> Two things worth recording from doing it. `print` output really does
> arrive as an `Output` EVENT and land in the transcript -- the screen runner
> asserts it by counting lines, since a leak to lens's own stdout would make
> the screen taller than its geometry, and it separately fails if anything
> reaches stderr. And `LocalSession` was reporting an appended clause as
> `replaced`, which told the user their first clause was gone; `replaced` is
> now reserved for `ReplacePredicates`, which needs engine item E3 and
> answers `Failed` rather than pretending.

| # | Criterion |
| --- | --- |
| G2.1 **[done]** | Against a real engine: define a fact, query it, see the binding. Golden screen. |
| G2.2 **[done]** | A parse error produces a `Diagnostic` rendered with file, line, column and the offending line — matching what `unify-run` already prints. *(2026-09-07: `<session>:1:1: parse error`, the offending line and a caret. Arrives as data through E10's `DiagnosticSink`; lens's stderr stays empty, which the screen runner asserts.)* |
| G2.3 **[done]** | `print`/`emit` output appears in the transcript and **not** on lens's stdout (this is E4's user-visible proof). *(2026-09-07: asserted structurally -- the rendered screen must be exactly as many lines as the geometry has rows, so anything printed past the renderer fails the test.)* |
| G2.4 | A query producing 10,000 solutions leaves the UI responsive: the transcript holds `initialDemand` rows and the key-script continues to be serviced. Asserted by the script completing within a wall-clock bound. |
| G2.5 | Re-running past input in place (`F5` on an old transcript line) re-issues the identical request. |
| G2.6 | `cancel` on a running query always reaches `QueryStatus::Aborted` and the UI detaches — with the status line stating the v1 limitation (plan §6) when the engine cannot actually stop. |
| G2.7 **[done]** | The status line reports demand as `buffered` while engine item E11 is outstanding, rather than implying flow control that does not exist ([SESSION-API.md](SESSION-API.md) §3.1). *(2026-09-07: read from `describe()` rather than hard-coded, so the day E11 lands the status line stops saying it without anyone editing that line. It also states the cancel limitation: `cancel detaches only`.)* |
| G2.8 | History persists to `$HOME/.unify_history` in the existing REPL's format, and a history file written by `unify-run -i` loads in lens and vice versa. |
| G2.9 | `M-x transcript.write-file` produces the transcript as text. |
| **H** | Every transcript command has an `M-x` entry and a help topic; `F1` on a builtin name under the cursor opens that builtin's extracted topic. |

## G3 — Browser

*Smalltalk's system browser, on a clause database.*

| # | Criterion |
| --- | --- |
| G3.1 | Catalogue shows module → `(name, arity)` → count, with builtins and synthesized clauses as collapsed, counted stubs, driven by provenance (E1) — never by name prefix. |
| G3.2 | Selecting a key loads its source into the source panel via `source`. |
| G3.3 | Editing and `F3` redefines it under `ReplacePredicates`: the catalogue count updates, the old clauses are gone, other predicates are untouched, and the transcript reports `name/arity replaced (n clauses)`. |
| G3.4 | `WorldChanged` refreshes the catalogue with no panel polling (asserted by request-count on the session). |
| G3.5 | Filter-as-you-type over a catalogue of 5,000 keys stays within the frame budget. |
| G3.6 | **Editor.** A property test applying 10,000 random edit operations (insert, delete, kill, yank, undo, redo) never corrupts the buffer and always round-trips to the same text under full undo. A width corpus of CJK and combining-mark clause heads renders with correct column alignment. Completion offers catalogue predicates, builtins and in-scope variables. |
| G3.7 | **Growth under redefinition.** Redefining one predicate 1,000 times leaves query latency and resident memory within a stated bound. Clauses are tombstoned, never removed (`vault-unify.hpp`: "A clause cannot be removed from an execution state"), and lookup is a linear scan until ROADMAP Phase 3's indexing lands — so interactive redefinition monotonically degrades the database. This gate is what forces E2's catalogue index to be a real `(name, arity)` index and not a view, and it is the test that says when Phase 3 became mandatory rather than optional. |
| G3.8 | **World undo.** `Ctrl-z` after a `ReplacePredicates` redefinition restores the previous clauses in their original trial order; an entry spanning a query side effect is marked non-undoable rather than silently half-applied. |
| **H** | Help topics for the browser and for the three overwrite policies exist and are reachable by `F1` from the source panel with the policy selector focused. |

## G4 — Image

*Requirement (b), completely.* All seven obligations of
[IMAGE-FORMAT.md](IMAGE-FORMAT.md) §7 are gate criteria; restated in short:

| # | Criterion |
| --- | --- |
| G4.1 | Round trip: `mediaplayer.ufy` → save → load in a fresh session → golden query output byte-identical to the original run. |
| G4.2 | Asserted runtime facts survive save/load (catalogue and answers identical). |
| G4.3 | A program using `for`/`foreach`/`if` round-trips, and its `@module` block contains the original text, not `__fe__` clauses. |
| G4.4 | **Edit-then-save composes.** Redefine a predicate in place (G3.3), save, load: the loaded world matches the live one including trial order. *This is the case the first draft of the format could not answer, and it is the reason `@changes` exists.* |
| G4.5 | **Retraction survives.** Retract a module-defined fact, save, load: it stays gone. |
| G4.6 | `ReplacePredicates` insert replaces exactly the keys the incoming text defines and nothing else. |
| G4.7 | An insert whose text fails to parse leaves the world bit-identical — nothing retracted, nothing added, diagnostics reported. Requires engine item E12 (staged parse); the gate does not pass on a best-effort approximation. |
| G4.8 | An insert containing a top-level `query { … }` block is **rejected** with a diagnostic and nothing applied — the un-rollback-able case ([IMAGE-FORMAT.md](IMAGE-FORMAT.md) §5.1). |
| G4.9 | Imports inside module text are not re-followed on load, and a post-load `import` of a module already in the image does not duplicate it — on a machine where the original imported files do **not** exist (engine item E13). |
| G4.10 | `@startup` runs on load, output reaches the transcript, and a *failing* startup goal reports without aborting the load. |
| G4.11 | `#!ufy-image 2` is refused with a clear message and no partial read. |
| G4.12 | `load` over a world with unsaved runtime state confirms first; declining changes nothing. Repeated `load` 100 times leaks no threads and no memory (engine item E9). |
| G4.13 | **Journal recovery.** `SIGKILL` lens mid-session; restart; the recovered world equals the pre-kill world. |
| G4.14 | **Compaction preserves behaviour.** `save` and `save --compact` of the same world load to worlds answering the golden query set identically. |
| G4.15 | An interrupted `save` leaves the previous image intact (write-to-temp-and-rename). |
| **H** | Help pages for save/load/insert and for the startup goal, reachable by `F1` from the image panel; the confirm dialog links to the `load` topic. |

## G5 — Inspect

| # | Criterion |
| --- | --- |
| G5.1 | Solutions table renders bindings; scrolling past the last row issues exactly one `demand` (not one per keystroke). |
| G5.2 | Inspector expands a selected binding as a term tree; a `truncated` node is marked and expanded via `inspect(q, index, path, budget)` — **not** by re-running the query, which is nondeterministic and may have side effects. Requires the query to be retained (G0.10). |
| G5.3 | Diagnostics panel navigates: Enter moves the source panel to the reported file/line/column. |
| G5.4 | ~~**Floor, stated explicitly:** with the engine's current `map<string,string>` solutions, the inspector renders `Str` leaves and the test asserts *that*.~~ **Floor raised 2026-09-08 — engine item E7 landed.** Bindings arrive as trees (`Cons`, `Array`, `Map`, `Int`, `Atom`, `Var`), `Capabilities::structuredSolutions` is `true`, and the contract suite's truncation case runs against the real engine instead of skipping. The prediction held exactly: **no session API change was required** — only the adapter and the inspector's richness moved, which is what the boundary was drawn for. Two obligations the floor did not mention, both now met: an unbound variable arrives as a `Var` under the caller's own name (SESSION-API §3), asserted unconditionally for every subject; and a value the budget cut is marked, since a compound rendered without its arguments is a *different* term shown as a whole one. See [E7-STRUCTURED-VALUES.md](E7-STRUCTURED-VALUES.md). |
| **H** | `F1` in the inspector explains the term kinds and the truncation marker. |

## G6 — Debug

| # | Criterion |
| --- | --- |
| G6.1 | With a debug-capable core, a breakpoint on a predicate halts and the goal stack renders. |
| G6.2 | Stepping advances one goal; the source panel highlights the current goal. |
| G6.3 | With a core reporting `debug: false` in `describe()`, the trace panel renders "unavailable" with a reason, and `setTrace` answering `Failed` is handled, not crashed (contract criterion, exercised via `FakeSession` too). |
| G6.4 | Trace output is demand-driven — a runaway trace does not exhaust memory. |
| **H** | Debugging help topic covering breakpoints, stepping and the capability limitation. |

## G7 — Remote

*The proof that requirement (c) was met rather than intended.*

| # | Criterion |
| --- | --- |
| G7.1 | The **unmodified** contract suite passes against `ProxySession` over loopback. |
| G7.2 | Two-process demo: `unify-lensd` on one port, `unify-lens --connect`; the G2, G3 and G4 golden screens reproduce identically over the socket. That the *screens* are identical is the assertion — it means no panel behaves differently when remote. |
| G7.3 | Killing the daemon mid-query marks the session degraded in the status line; lens stays alive and interactive. |
| G7.4 | Restarting the daemon and resubscribing either resumes at `seq` or reports expiry, and the world view is consistent either way. |
| G7.5 | Image paths resolve on the daemon's filesystem, and the image panel labels them `remote:<host>` ([IMAGE-FORMAT.md](IMAGE-FORMAT.md) §6). |
| G7.6 | No behavioural difference between local and remote appears anywhere in `model/` or `panels/` — asserted by those directories containing no reference to the session implementation. |
| G7.7 | **Security.** `unify-lensd` binds loopback by default; a non-loopback `--listen` without a credential refuses to start; an unauthenticated connect is rejected; `--help` documents SSH tunnelling as the supported remote transport and states that TLS is not implemented ([SESSION-API.md](SESSION-API.md) §7). |
| G7.8 | G7.2's "identical screens" criterion is asserted at the **model** level as well as the grid level, so it does not silently depend on a total event order that a future concurrent core may relax to per-query order. |
| **H** | Help topic for remote sessions covering path resolution and degraded state. |

## G8 — Platform

| # | Criterion |
| --- | --- |
| G8.1 | CI builds and runs the suite on **Linux**, **macOS** and **Windows 11** under GitHub Actions. The `.forgejo/` twin gains the lens job for every platform it has runners for — Linux at minimum — and an explicit comment naming the platforms it cannot cover, so the twins stay legibly in sync rather than silently divergent. (The README's sync requirement assumes parity of available runners; where that is untrue, saying so beats a job that cannot run.) |
| G8.2 | Windows Terminal / PowerShell is the supported path: VT processing enabled at startup, goldens identical to Linux at the same geometry. |
| G8.3 | Git Bash / mintty: the no-real-console detection fires and prints the pinned one-line message naming `winpty` and the `unify-run -i` fallback ([ARCHITECTURE.md](ARCHITECTURE.md) §6.3). Golden-pinned. **A broken screen is a gate failure; a clear refusal is a pass.** |
| G8.4 | Monochrome tier is legible and every selection visible; asserted by the goldens being colour-independent by construction. |
| G8.5 | Clean under `UNIFY_SANITIZE=address,undefined` on Linux and macOS, with zero leaks on a scripted session — matching the engine's zero-leak bar. |
| **H** | Platform notes topic, including the Git Bash limitation, shipped in the built-in help. |

## G9 — Extension

*Requirement (f), tested rather than asserted.*

| # | Criterion |
| --- | --- |
| G9.1 | A real module (the test-runner panel over the golden corpus) is added as one new directory plus one line in `lens/CMakeLists.txt`. **`git diff --stat` shows no other existing file modified.** That diff is the gate. |
| G9.2 | It obtains, with no lens change: a menu entry, a hint-line key, an `M-x` entry, a config-file rebinding, help topics, a tile, and session events. |
| G9.3 | Module conformance test: `view` performs no I/O, `fold` does not block, both are pure over their state. |
| G9.4 | Its golden screens at 80×24 and 120×40 are recorded by the standard harness with no harness change. |
| G9.5 | Removing the module's `CMakeLists.txt` line builds cleanly and its commands, menu entries and help topics all disappear. |
| **H** | Its help topics are reachable via `F1` and appear in the generated command page — obtained from the interface, not written into lens. |

---

## Definition of done

All ten gates closed, plus:

- Engine goldens byte-identical to `a20ddda`, on every platform.
- Zero warnings under `-Wall -Wextra`; sanitizer-clean; zero leaks.
- `unify-run`'s batch mode and existing REPL unchanged and still shipped —
  lens is an addition, and the CI harness depends on `unify-run` staying
  exactly as it is.
- `lens/README.md` written in the style of `unify/README.md`, and
  `unify/ROADMAP.md` updated with engine items E1–E14 — several of which
  (E9 world reset, E11 per-solution emission, E14 the 5.1 concurrency
  subset) are Phase 3/5 work this plan pulls forward, and should be
  recorded there as such rather than as tooling.
