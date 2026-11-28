# Acceptance gates

Companion to [`../lens-text-mode-environment.md`](../lens-text-mode-environment.md).
Plan requirement (e): development is test-driven, and progress is measured
by gates whose criteria are executable.

> ## Where the gates stand — 2026-11-28
>
> | Gate | | |
> | --- | --- | --- |
> | **G0** Boundary | **CLOSED** 2026-09-06 | contract suite 57 passed / 0 failed / 3 skipped, over three subjects |
> | **G1** Shell | **CLOSED** 2026-09-07 | tiling, four layouts, degradation, `--script`, commands, help, palette |
> | **G2** Transcript | **CLOSED** 2026-11-27 | batches, cancel, `F5` re-run, shared history, write-file, extracted help |
> | **G5** Inspect | **CLOSED** 2026-11-28 | Solutions table and demand, Inspector tree and `inspect`, Diagnostics to Source |
> | **G3** Browser | **PARTIAL** | G3.1–G3.5 and H done; G3.6 (the editor), G3.7 (growth) and G3.8 (world undo) open |
> | **G4, G6–G9** | not started | |
>
> Engine items **E1, E2, E3, E4, E5, E7, E10, E14, E16** are done; **E6,
> E9, E11, E12, E13, E15** are not. `ctest` is 31/31 on `build/unify` and
> 74/74 on `build/lens`; CI additionally builds and tests on Windows/MSVC.
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
> **Closed 2026-11-27.** G2.4, G2.5, G2.6, G2.8, G2.9 and H, each with a
> note below saying what it took. Every one was proven able to fail before
> it was ticked; G2.4 was already true, which is the case that proof exists
> for. Three new test homes came with it: `lens-session` (a Model and a
> SessionBridge over a real engine -- the first lens test that can say
> anything about the pair), the readline cases in `lens-pty-interaction`,
> and `lens-help-extract`.
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
| G2.4 **[done]** | A query producing 10,000 solutions leaves the UI responsive: the transcript holds `initialDemand` rows and the key-script continues to be serviced. Asserted by the script completing within a wall-clock bound. *(2026-11-27: it already held, which is the one thing a test written after the fact cannot be trusted to show -- so it was proven able to fail: with the bridge's `initialDemand` raised to 100,000 the case reports "expected 20 solution rows, found 10000". The first attempt at that proof failed for the WRONG reason, "the solve was never issued", because a query that delivers everything clears `liveQuery` before anyone can ask how many rows it got; the rig now records the id at issue time. Asserted in a new suite, `lens-session`, a Model and a SessionBridge over a real LocalSession driven the way `--script` drives them -- the first lens test that can say anything about the pair. Two things the criterion did not ask for and the user needs: a batch that ends with more to come now SAYS so in the transcript, since otherwise its last row looks exactly like the last row of the answer, and `query.more` fetches the next batch.)* |
| G2.5 **[done]** | Re-running past input in place (`F5` on an old transcript line) re-issues the identical request. *(2026-11-27: the transcript had no cursor over its past at all -- a `scrollBack` field nothing ever changed -- so most of this was giving it one. PageUp and PageDown move it from input line to input line, never onto a solution row, because a row is not something you can run; the arrows stay with the history, which is the other way to get an old line back when you want to edit it first. `F5` (`transcript.rerun`) goes through `submitTranscriptLine()`, the function Enter uses, so the request is identical by construction. Proven able to fail by re-running the RENDERED line instead of the stored source: `"?- colour( $x )"` against `"colour( $x )"` -- exactly the reconstruction `TranscriptEntry::source` exists to prevent. The cursor scrolls through the shared rule in `scroll.hpp` and reports a viewport to the interaction suite, whose invariants caught a cursor-only offset at once ("scrolled from 27 to 26 although the cursor was already visible"); PageUp, PageDown, F5 and C-c joined the seeded random walk. Two things a user would have hit: a half-typed line survives an F5 side trip, and only one cursor is drawn -- the prompt's block disappears while a past line is highlighted. Binding F5 put `F5 Run line` on the hint line, which is the only change in the 19 re-recorded screens.)* |
| G2.6 **[done]** | `cancel` on a running query always reaches `QueryStatus::Aborted` and the UI detaches — with the status line stating the v1 limitation (plan §6) when the engine cannot actually stop. *(2026-11-27: `query.cancel` on `C-c`, and `C-g` (`app.abort`) now aborts a live query rather than saying "nothing to abort". The UI detaches FIRST, before the core answers -- a front end that waited for `Aborted` would be as stuck as the engine on a goal that never ends, which is the case cancel exists for. Rows that arrive for a detached query are dropped; its terminal status is still shown, as the core's own account of what it did. A defect found on the way: ANY terminal status used to mark the transcript finished, so a superseded query ending would have reported the current one idle. Only the live query's end ends the live state now. The message says "detached · this engine cannot stop a running goal yet" while `realCancel` is false, beside the status line's existing "cancel detaches only".)* |
| G2.7 **[done]** | The status line reports demand as `buffered` while engine item E11 is outstanding, rather than implying flow control that does not exist ([SESSION-API.md](SESSION-API.md) §3.1). *(2026-09-07: read from `describe()` rather than hard-coded, so the day E11 lands the status line stops saying it without anyone editing that line. It also states the cancel limitation: `cancel detaches only`.)* |
| G2.8 **[done]** | History persists to `$HOME/.unify_history` in the existing REPL's format, and a history file written by `unify-run -i` loads in lens and vice versa. *(2026-11-27: the format is readline's `write_history()` as unify-repl.cpp calls it -- one line per entry, no timestamps, the newest 1000 kept to match its `stifle_history( 1000 )` -- parsed and formatted by pure model functions, with the file I/O in `app/`. The save goes to a temporary and is renamed over the file, because the file is shared and truncating it would cost the user the OTHER program's history too. Interactively the default is `$HOME/.unify_history`; under `--script` there is none unless `--history` names one, since a test that wrote the user's real history would be a test that edits the user's data. "Vice versa" is asserted for real, in both directions, by driving `unify-run -i` on a pseudo-terminal -- the only way readline engages -- in `lens-pty-interaction`; proven able to fail by not loading the file, and by writing a format readline does not read. That needed readline, which neither this machine nor CI had: without it `unify-run -i` keeps no history file at all, so the cases SKIP with that reason, and the lens CI job now installs `libreadline-dev`. Two things found on the way. A line recalled from unify-run is in ITS syntax, and `? colour( $x );` would have reached lens's `;` heuristic and been sent as a definition -- so lens now accepts the `?` shorthand. And the pty harness's `waitExit()` spun through its whole budget in milliseconds once the child closed the terminal, because `poll()` reports a hang-up at once; the existing quit case only got away with it because lens exits fast, and unify-run, which writes its history after closing the terminal, did not.)* |
| G2.9 **[done]** | `M-x transcript.write-file` produces the transcript as text. *(2026-11-27: the command needs a file name and lens had no way to ask for one, so the palette's tile gained a prompt mode -- a label, one line of input, Enter accepts, Esc and C-g cancel, C-u clears the suggested `transcript.txt`. UI.md section 4 makes the minibuffer the one place lens asks for text; a second input surface would have behaved slightly differently and been found out later. Writing the file is lens's own I/O, so it does not go through the session: both loops offer each request to `performLocalRequest()` first, which means the command works with no engine and, once G7 moves the engine to another host, will not write the file THERE. Asserted twice: on the model (the request's path and text, byte for byte) and end to end by `check-transcript-write.sh`, which drives the shipped binary with a real engine into a fresh directory whose path has a space in it -- proven able to fail by unhooking the write: "no file was written". Recording the golden exposed an existing G2.2 wart: a goal that does not parse is reported against the engine's wrapper, `query {` with the caret at 1:1, rather than against what the user typed.)* |
| **H** **[done]** | Every transcript command has an `M-x` entry and a help topic; `F1` on a builtin name under the cursor opens that builtin's extracted topic. *(2026-11-27: "extracted" was the real work -- UI.md section 5.3's third source of help did not exist. `tools/help-extract.cpp` runs at build time and makes one topic per builtin from SPEC.md section 9's bold signature paragraphs, one per reserved word from LANGUAGE.md's reference table, and a `language` index; `F1` in the transcript answers for the word the cursor is in or just after, falling back to the transcript's own page. Section 5.3 names the honest limit -- an extractor is a parser and can drift -- so it is strict: a missing section, a paragraph-opening bold line that is not a signature, a short table row, an empty source or a name documented twice stops the build with `file:line`, and `check-help-extract.sh` holds one fixture per refusal so the strictness stays real. It refused the real SPEC.md on its first run, and was wrong to: a wrapped line there starts with `**byte**`, which is text, so only a bold line that OPENS a paragraph is taken for a heading -- the same fixture now pins that. Presence is checked against the one list that cannot be wrong about what exists, the engine's own `describe()`: proven able to fail by renaming `emit` in SPEC.md, which reports "builtin 'emit/1' has no help topic -- document it in unify/SPEC.md section 9". The topics are the documents' words as they stand, and SPEC.md section 9 is written for implementers -- file paths, class names -- so a user pressing F1 on `print` gets the truth at a reader's cost; a user-facing paragraph per builtin in LANGUAGE.md is the fix, and it belongs to the documents, not to lens.)* |

## G3 — Browser

*Smalltalk's system browser, on a clause database.*

> **Progress note (2026-11-28).** G3.1, G3.2, G3.4 and G3.5 are done: the
> read-only browser. They are asserted on the model in a new suite,
> `lens-browser`, and G3.2 and G3.4 are asserted again against the real
> engine in `lens-session`. Every guard was proven by breaking it.
>
> **Later the same day: G3.3 and H too**, which took two engine items
> first. E3 (predicate-scoped retract) was the expected one. E5 (source
> retention) was the surprise. The engine's `source` printed its internal
> debug form (`warm( $c (VT29) ) :- colour( $c (VT29) ), unify( ... ).`),
> which neither reads as the language nor parses back. So "edit it and
> press F3" could not have worked on any definition, however good the
> editor. Both items are recorded in `unify/ROADMAP.md`.
>
> Open: G3.6 (the editor proper — the Source panel is a line editor for
> now, with no undo); G3.7, which needs a measurement before anyone agrees
> a bound; and G3.8 (world undo).
>
> One mechanism came with it that later panels will reuse. A session
> event can now ask for something: `Model::takeRequests()` holds what
> `foldSession` decided, and every loop that folds events issues it. That
> is how a `WorldChanged` asks for a listing without anything polling.

| # | Criterion |
| --- | --- |
| G3.1 **[done]** | Catalogue shows module → `(name, arity)` → count, with builtins and synthesized clauses as collapsed, counted stubs, driven by provenance (E1) — never by name prefix. *(2026-11-28: groups are `<transcript>`, one per module file, `asserted`, `synthesized` and `builtins`, read off each entry's `Origin`. A user predicate called `__cache` is listed under `<transcript>`, and the test fails when the group is decided by a `__` prefix instead. The two stubs start closed and open to show what they counted. A paged listing is shown only once its last page arrives, so the panel never presents half a world as the whole.)* |
| G3.2 **[done]** | Selecting a key loads its source into the source panel via `source`. *(2026-11-28: Enter on a predicate asks `source` for exactly that key and keeps the keyboard in the catalogue. That is the system-browser stance: you walk the list and the definition follows. A Source tile opens beside the catalogue when the layout has none. An answer for a key no longer selected is dropped. What arrives is the engine's clauses printed back, not the text as typed: that is engine item E5, and the panel says so on its second line.)* |
| G3.3 **[done]** | Editing and `F3` redefines it under `ReplacePredicates`: the catalogue count updates, the old clauses are gone, other predicates are untouched, and the transcript reports `name/arity replaced (n clauses)`. *(2026-11-28: against the real engine. Deleting `colour( green );` from colour/1's definition and pressing F3 leaves colour/1 with one clause in the catalogue AND in the engine's answers, warm/1 untouched, and the transcript line `-- colour/1 replaced (1 clause)`. The count needed a new field on `Defined`, `replacedClauses`, parallel to `replaced`; SESSION-API.md is amended. An edit that does not parse changes nothing: the clauses the parser had already appended are taken back, the old definition stands, the edit stays in the panel, and the status line says so. That case first passed vacuously. The core sent `WorldChanged` only for a clean define, so lens's catalogue never refreshed and kept showing the old count whatever the engine held. The test now asks the engine, and the core announces every change of generation. Seven mutations, each caught.)* |
| G3.4 **[done]** | `WorldChanged` refreshes the catalogue with no panel polling (asserted by request-count on the session). *(2026-11-28: one listing per `WorldChanged`. Changes that arrive while a listing is in flight coalesce into exactly one more. A hundred keystrokes with no change ask for none, in the model test and against the real engine. The first listing is asked for when a session's capabilities arrive, so a model with no engine asks nothing.)* |
| G3.5 **[done]** | Filter-as-you-type over a catalogue of 5,000 keys stays within the frame budget. *(2026-11-28: the budget is 50 ms, the interactive loop's poll interval, measured as the WORST keystroke of fold plus a full render. The first implementation took 51 ms and failed: it re-sorted every key on each of the several calls a keystroke makes. Sorting once per listing brought it to 20 ms. Asserted only in optimised builds; an unoptimised or sanitizer build skips the timing check and says so, since it would measure the build. A filter searches the stubs too, because it is how you find a builtin.)* |
| G3.6 | **Editor.** A property test applying 10,000 random edit operations (insert, delete, kill, yank, undo, redo) never corrupts the buffer and always round-trips to the same text under full undo. A width corpus of CJK and combining-mark clause heads renders with correct column alignment. Completion offers catalogue predicates, builtins and in-scope variables. |
| G3.7 | **Growth under redefinition.** Redefining one predicate 1,000 times leaves query latency and resident memory within a stated bound. Clauses are tombstoned, never removed (`vault-unify.hpp`: "A clause cannot be removed from an execution state"), and lookup is a linear scan until ROADMAP Phase 3's indexing lands — so interactive redefinition monotonically degrades the database. This gate is what forces E2's catalogue index to be a real `(name, arity)` index and not a view, and it is the test that says when Phase 3 became mandatory rather than optional. *(Baseline measured 2026-11-28, bound NOT yet agreed. One 10-clause predicate redefined under `ReplacePredicates` (E3), queried 20 times at each checkpoint, -O2, this machine. After 0, 100, 500 and 1,000 redefinitions: the store holds 24 / 1,024 / 5,024 / 10,024 clauses; `p( $x )` takes 0.038 / 0.040 / 0.080 / 0.204 ms; an unrelated `other( $y )` takes 0.039 / 0.040 / 0.100 / 0.189 ms; resident memory is 5.2 / 5.9 / 8.4 / 11.5 MB. So latency grows linearly with tombstones for EVERY predicate, since lookup scans the whole store, and each retired clause costs about 630 bytes, its kept source text (E5) included. A thousand edits is a fivefold slowdown at 0.2 ms; the same rate gives about 20 ms after a hundred thousand. Choosing the bound is the decision this criterion was written to force, and it is the maintainer's to take.)* |
| G3.8 | **World undo.** `Ctrl-z` after a `ReplacePredicates` redefinition restores the previous clauses in their original trial order; an entry spanning a query side effect is marked non-undoable rather than silently half-applied. |
| **H** **[done]** | Help topics for the browser and for the three overwrite policies exist and are reachable by `F1` from the source panel with the policy selector focused. *(2026-11-28: topics `catalogue`, `source` and `overwrite-policies`. The selector is a line of its own under the Source panel's header, reached with Up from the first line. Left and Right change the policy, and F1 there opens the policies page; F1 in the text opens `source`. Fail is explained as refused until engine item E12, which is what the core does.)* |

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

> **Closed 2026-11-28.** G5.1, G5.2, G5.3 and H, each with a note below. All
> are asserted on the model in a new suite, `lens-inspect`, with hand-built
> session events. G5.1 and G5.2 are asserted again in `lens-session`
> against the real engine, and G5.2's golden screen (`inspect-tree`) is
> recorded from it. Every criterion was proven able to fail by breaking
> the code it names. The mutations, one per guard, are listed against each
> criterion.
>
> Three things outside the panels that doing it turned up:
>
> - **lens was leaking every query it ran.** The bridge has always solved
>   with `retain = true` and never released anything, so the core held
>   every query's solutions for the life of the session. That is exactly
>   the debt G0.10 says retention creates. lens now retains exactly the
>   query the Solutions and Inspector panels show, and releases the one
>   they stop showing (`Model::beginQuery` returns it,
>   `SessionBridge::issue` pays it). `lens-session` asserts the core's
>   `retainedQueries` never exceeds 1 over six queries, and that test fails
>   when the release is removed.
> - **`Expanded` did not name its query.** `EventHeader::query` is
>   documented as "set on every query-attributable event". Neither
>   `LocalSession` nor `FakeSession` set it on an expansion, so a front end
>   could not tell which tree an answer belonged to. The contract suite
>   now asserts it for all three subjects (it failed for all three first),
>   and both sessions set it. No engine golden moved: the event never
>   reaches stdout.
> - **The Source panel is honest about being half there.** G5.3 asks for
>   "Enter moves the source panel to the reported file/line/column", and
>   it does. What it can show there is what a `Diagnostic` carries: the
>   location, the message, the one offending line and a caret. Showing
>   the whole definition needs G3's `source` request, and the panel says
>   so on its last line.

| # | Criterion |
| --- | --- |
| G5.1 **[done]** | Solutions table renders bindings; scrolling past the last row issues exactly one `demand` (not one per keystroke). *(2026-11-28: one row per solution, one column per variable, and a footer that tells "20 so far" from "20 solutions". `ResultsState::demandPending` holds the one outstanding demand. Five Downs past the end ask once, and the next batch's arrival re-arms it. `query.more` goes through the same `demandMore()`, so the two cannot both ask. Proven by removing the guard, which fails both model cases. The real-engine case cannot see the guard: its rig settles the engine after every key, so a demand is always answered before the next key. It pins the end-to-end path instead, and fails when the bridge drops the demand.)* |
| G5.2 **[done]** | Inspector expands a selected binding as a term tree; a `truncated` node is marked and expanded via `inspect(q, index, path, budget)` — **not** by re-running the query, which is nondeterministic and may have side effects. Requires the query to be retained (G0.10). *(2026-11-28: a row's path is the `inspect` path itself — binding index first, then indices into the value — so nothing translates between the two. A cut node keeps the transcript's U+2026 mark, and a compound cut to no arguments is still expandable, because `g( … )` is not the atom `g`. Opening it asks `inspect` once and shows "(fetching…)"; the answer is grafted only where a pending request matches its query and path, and the table sees the graft too, because there is one copy of the values. Against the real engine an 11-deep term is cut at the core's depth of 8 and opened to the bottom with one `solve` in all. Proven by six mutations: cut compounds not expandable, inspect asked twice, inspect turned into solve, a graft without a matching request, no re-rooting on a new selection, and results folded only when a transcript exists. That last one is the trap waiting in the existing `foldSession`, which returned early when the layout had no transcript, and the `debug` layout has an Inspector and no Transcript.)* |
| G5.3 **[done]** | Diagnostics panel navigates: Enter moves the source panel to the reported file/line/column. *(2026-11-28: every `Diagnostic` is listed with the transcript's own headline. Enter sets the Source panel's location and focuses it, opening a Source tile beside the largest one when the layout has none, as the `run` layout does not. See the note above for what the Source panel can and cannot show yet.)* |
| G5.4 | ~~**Floor, stated explicitly:** with the engine's current `map<string,string>` solutions, the inspector renders `Str` leaves and the test asserts *that*.~~ **Floor raised 2026-09-08 — engine item E7 landed.** Bindings arrive as trees (`Cons`, `Array`, `Map`, `Int`, `Atom`, `Var`), `Capabilities::structuredSolutions` is `true`, and the contract suite's truncation case runs against the real engine instead of skipping. The prediction held exactly: **no session API change was required** — only the adapter and the inspector's richness moved, which is what the boundary was drawn for. Two obligations the floor did not mention, both now met: an unbound variable arrives as a `Var` under the caller's own name (SESSION-API §3), asserted unconditionally for every subject; and a value the budget cut is marked, since a compound rendered without its arguments is a *different* term shown as a whole one. See [E7-STRUCTURED-VALUES.md](E7-STRUCTURED-VALUES.md). |
| **H** **[done]** | `F1` in the inspector explains the term kinds and the truncation marker. *(2026-11-28: topics `inspector` (every kind, the U+2026 mark, and why a cut node is fetched rather than re-solved), `solutions`, `diagnostics` and `source`. F1 opens each from its own panel. The test checks the inspector page names all seven kinds and shows the mark itself, and fails when F1 falls back to the panels page.)* |

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
