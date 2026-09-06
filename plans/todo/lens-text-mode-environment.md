# Plan — `unify-lens`: a text-mode operating environment for Unify

Status: **proposed**, drafted 2026-09-06; **revised 2026-09-06** after an
independent architecture review (see §12 for what the review changed).
Scope: a new sibling program to `unify/`, living in `lens/`, plus a small,
named boundary in the engine that it (and every future front end) talks to.

Companion architecture documents, all under [`lens/`](lens/):

| Document | What it settles |
| --- | --- |
| **[HANDOFF.md](lens/HANDOFF.md)** | **Start here if you are implementing this** — read order, first task, the traps found while reading the sources |
| [ARCHITECTURE.md](lens/ARCHITECTURE.md) | Process/module structure, the three core-adapter shapes, threading and data-flow rules |
| [SESSION-API.md](lens/SESSION-API.md) | The core boundary: 15 requests, 12 events, the value model, and why each survives BEAM and a network hop |
| [IMAGE-FORMAT.md](lens/IMAGE-FORMAT.md) | `save` / `load` / `insert`, the `.ufyimg` bundle, the startup goal, overwrite policies |
| [UI.md](lens/UI.md) | Tiling model, the ten v1 panels, the editor, keymaps, command table, the IDE-module extension interface |
| [ACCEPTANCE.md](lens/ACCEPTANCE.md) | The ten TDD gates, each as executable acceptance criteria |

---

## 1. Why

Unify today is a batch runner plus a line-oriented REPL (`unify-run -i`,
ROADMAP Phase 4). That is enough to *check* a program and not nearly enough
to *develop* one. The working mode Unify actually wants is the one Prolog,
Lisp and Smalltalk converged on independently: you grow a live world by
poking at it, you look at what is in there, you fix a definition in place,
and you keep the world across sessions.

Three things are missing for that, and they are the whole of this plan:

1. **A live world you can see.** A tiled, always-on view of the clause
   database, the query in flight, the bindings it produced, the diagnostics
   it raised — instead of a screenful of scrollback you have to remember.
2. **A world you can keep.** Save the environment to an image, restore it,
   or graft a file into it — the Smalltalk image loop, which is the reason
   incremental development there ever felt safe.
3. **A boundary the engine can be swapped behind.** The intent is that a
   BEAM-based Unify, or a Unify on another machine, can back the same
   environment. That is only possible if the front end never touches an
   engine object — which today it does, directly and pervasively.

## 2. Design lineage (what we are and are not copying)

- **Smalltalk-80** — the *image*, and the system browser: a
  category/class/method spine you navigate rather than a file you scroll.
  Unify's spine is module → predicate name/arity → clause.
- **Oberon** — *tiled, never overlapping*; every visible region is a text
  viewer; text is executable, so a command is something you point at and
  run, not something you retype. Lens keeps tiling absolute and makes the
  transcript's past input re-runnable in place.
- **Emacs (pre-CUA)** — buffers are decoupled from windows; a keymap is
  data; every binding is a named command and every named command is
  reachable without a binding (`M-x`). This is what makes the environment
  extensible by strangers, and it is the model for the module interface.
- **Turbo Pascal / Turbo C IDEs** — a menu bar you can reach with one key,
  a function-key hint line at the bottom, modal dialogs that are simply
  more tiles, and a fixed, learnable screen. This is what makes it
  *immediately* usable by someone who has never seen it.

We are **not** copying: overlapping windows, mouse-first interaction,
Smalltalk's "the image is the only source of truth" (see
[IMAGE-FORMAT.md](lens/IMAGE-FORMAT.md) §2 — source text stays primary), or
Emacs's extension-language-in-the-editor ambition (v1 modules are compiled
C++, see [UI.md](lens/UI.md) §7).

## 3. Deliverables

| # | Deliverable | Where |
| --- | --- | --- |
| D1 | `unify::session` — the core boundary: abstract `Session`, value model, request/event types | `unify/include/vault-unify-session.hpp` |
| D2 | `LocalSession` — in-process adapter over `RuntimeContext`, conforming to D1's async contract even though it could cheat | `unify/src/vault-unify-session-local.cpp` |
| D3 | Core capabilities the boundary needs and the engine lacks: clause provenance, output redirection, predicate-scoped retract, definition catalogue, canonical fact printing | `unify/src/` (see §5) |
| D4 | Image support: `.ufyimg` writer/reader, overwrite policies, startup goal | `unify/src/vault-unify-image.cpp` |
| D5 | `unify-lens` — the text-mode environment | `lens/` |
| D6 | `ProxySession` + a reference transport, and a `unify-lensd` head that serves `Session` over it | `lens/proxy/`, `unify/tools/` |
| D7 | Contract test suite runnable against *any* `Session` implementation | `unify/test/session/` |
| D8 | Golden-screen test harness for the TUI, in the style of `test/run-golden-test.sh` | `lens/test/` |
| D10 | World journal + undo stack, and the `.ufyjournal` crash-recovery sidecar | `lens/model/journal/` |
| D9 | Online help: contextual `F1`, help panel, hint line, and a build step extracting builtin/keyword topics from `LANGUAGE.md` + `SPEC.md` | `lens/help/`, `lens/panels/help/` |

## 4. The boundary, in one page

The full specification is [SESSION-API.md](lens/SESSION-API.md); the shape
is this.

```
            +-------------------+
            |    unify-lens     |    panels, layout, keymaps
            +---------+---------+
                      | unify::session::Session   (15 requests, 12 events)
       +--------------+--------------+---------------+
       |                             |               |
+------v------+            +---------v------+  +-----v---------+
| LocalSession|            |  ProxySession  |  |  BeamSession  |
| RuntimeCtx  |            |  transport     |  |  port/dist    |
+-------------+            +---------+------+  +---------------+
                                     |
                              +------v------+
                              | unify-lensd |  LocalSession behind a socket
                              +-------------+
```

Six rules make the three implementations interchangeable, and the *current*
`unify-run` breaks all six:

1. **No engine objects cross the boundary.** Not `Clause*`, not
   `UnifyContext*`, not `WorldPtr`. Only values and opaque ids.
2. **Every request is asynchronous and correlated.** A request returns a
   `RequestId` immediately; the answer arrives on the event stream. There
   is no `waitForEngineIdle()` barrier-job trick, because "the engine is
   idle" is not a question you can ask a cluster.
3. **Solutions are pulled, not pushed.** `solve` starts a query; `demand(n)`
   asks for at most *n* more solutions. A BEAM process producing solutions
   faster than a terminal can draw them must not build an unbounded mailbox,
   and a 40-row screen has no use for a million rows.
4. **All engine output is an event.** `print`, `emit` and diagnostics go to
   the event stream, never to the process's stdout — a remote engine's
   stdout is on the wrong machine.
5. **Every event carries `(seq, worldGeneration)`,** and every event a
   query could have caused carries its `QueryId` and a per-query
   `querySeq`. A front end that reconnects resumes from `seq`; a view that
   renders `worldGeneration` knows when it is stale; and two concurrent
   queries on a BEAM core do not produce unattributable output. All three
   are free in-process and load-bearing remotely.
6. **Every retained thing has a release.** Demand-driven delivery means the
   core holds undelivered solutions and their term trees; without an
   explicit `release`, a session leaks one retained query per query run.

Concretely, `parseExecuteSegment(it, itEnd, boost::function<...>, ...)`
— a synchronous call taking iterators into caller memory and a callback
invoked on the engine's worker thread — becomes `define(text, origin)`
returning a `RequestId`, answered by `Defined{added, replaced, diagnostics}`.

## 5. Engine work this requires

These are genuine additions to `vault-unify-core`, not adapter tricks.
**The engine work is the bulk of this plan, not a preamble to it** — an
independent review of the first draft found that the two largest items were
missing from the list entirely (E9, E14), and that two more had been
mis-sized as adapter work (E10, E12). The corrected list:

### 5.1 Required before G0

| # | Item | Why the boundary needs it |
| --- | --- | --- |
| E1 | **Clause provenance** — origin (module id, source span), kind (`module` / `asserted` / `builtin` / `synthesized`) | Today "not user code" is inferred from a `__` name prefix and a `dynamic_cast<SimpleBuiltinClause*>` (`unify-repl.cpp:489,497`). The catalogue, the source view, the image writer and compaction all need this to be a fact, not a heuristic. |
| E2 | **Definition catalogue** — `(name, arity) → {clauses, origin, generation}` | `listing`/`source`; the browser. Also the data structure ROADMAP Phase 3's first-argument indexing needs — build it once, use it twice. |
| E4 | **Output redirection** — a per-`RuntimeContext` sink for `print`/`emit`/trace instead of `std::cout` | Boundary rule 4. Note `print` writes `"print: " << s` (`vault-unify-clause-builtin-print.cpp:28`); the default sink must reproduce that byte-for-byte or the engine goldens break. |
| E10 | **Structured diagnostics** — parse and runtime errors as data, not formatted stderr text | `reportParseError` prints gcc-style text and `parseExecuteSegment` returns only a count (`vault-unify-runtime-context.cpp:99-102`); `SolveJob` keeps only a count and the *last* message (`vault-unify-solvejob.hpp:161,167`). The diagnostics panel navigates by file/line/column, so this is engine work. **The first draft wrongly implied the data already existed.** |
| E14 | **The ROADMAP 5.1 subset** — atomic id counters, locked `setTermDebugInfo`, reader-safe catalogue reads | Without the `waitForEngineIdle()` barrier, a `define` parse or `listing` read can overlap a running solve — exactly the race 5.1 exists to fix. The alternative (serialise everything behind the running query) would let one runaway query wedge the entire session. See [ARCHITECTURE.md](lens/ARCHITECTURE.md) §3.1. |

### 5.2 Required for image and inspection

| # | Item | Why |
| --- | --- | --- |
| E3 | **Predicate-scoped retract** | `ReplacePredicates`; redefining from the editor. Extends the existing tombstone/`retire()` mechanism. |
| E5 | **Module source retention** — keep each loaded segment's original text, keyed by module | Image fidelity: `for`/`foreach`/`if` desugar at parse time into `__fe__N` clauses (`vault-unify-parser.cpp:722,1099,1354`), so re-printing the database would emit machine output, not the program. |
| E6 | **Canonical fact printing** — `portray_clause` for ground asserted facts | The only part of an image with no source text. |
| E9 | **World reset** — `RuntimeContext::resetWorld()` | `load` replaces the world, and today a `RuntimeContext` binds one Engine and World for life while `~RuntimeContext` deliberately leaks the Engine (no shutdown path, ROADMAP 5.1). "Rebuild the context" would leak a worker thread per load. **Missing from the first draft entirely.** |
| E12 | **Staged parse and commit** — parse to a target other than `m_esRoot`, install on commit | `insert` atomicity. `parseExecuteSegment` is an interleaved parse-and-execute loop that appends clauses and *launches queries* mid-parse (`vault-unify-runtime-context.cpp:158-222`), and `ExecutionState::fork()` is documented dead code (`vault-unify.hpp:414,444`). **The first draft called this "implementable on the current engine"; it is not.** |
| E13 | **Import suppression + origin-keyed once-registry** | Images must not re-follow imports on load, and the current registry keys on `boost::filesystem::canonical()` — uncomputable on a machine lacking the files, which is the case images exist for. |
| E7 | **Structured solutions** | `SolveJob::SolutionMap` is `map<string,string>` (`vault-unify-solvejob.hpp:92`). **Already ROADMAP Phase 1, open.** Lens does not block on it — see §6. |
| E15 | **Solution retention until release** | `inspect` expands a truncated term. Solution term trees are freed with the job today, so expansion requires retaining them past completion, released by the `release` request. |

### 5.3 Deferred, with the front end told the truth meanwhile

| # | Item | Status |
| --- | --- | --- |
| E8 | **Query cancellation** | Needs ROADMAP 5.1 + Phase 3 bounded slices. See §6. |
| E11 | **Per-solution emission hook** | `performSlice()` runs to completion and solutions materialise at `onFinished`, so `demand` drains a fully computed buffer rather than throttling a producer. See §6. |

## 6. Three honest dependencies on unfinished ROADMAP work

None blocks the start of this plan; each is handled explicitly rather than
pretended away.

**Structured solutions (E7 / ROADMAP Phase 1).** The wire value model
([SESSION-API.md](lens/SESSION-API.md) §3) is defined now, in full. Until
Phase 1's item lands, `LocalSession` fills every binding as
`Value::Text{...}` from the existing `SolutionMap` strings, and the
inspector renders a leaf instead of a tree. The API does not change when the
engine catches up — only the adapter and the inspector's richness do. Gate
G5 states this explicitly as its floor.

**Demand (E11 / ROADMAP Phase 3).** `demand(n)` is the right API and, in
v1, not yet real back-pressure: `performSlice()` computes every solution
before `onFinished` fires, so the core buffers and `demand` drains. The
front end says `buffered` in the status line rather than implying flow
control (gate G2.7). The first draft applied the honesty rule below to
`cancel` and not to `demand`; both get it.

**Cancellation and shutdown (E8 / ROADMAP 5.1, 3).** The engine has one
worker thread, no shutdown path, and `performSlice()` runs a query to
completion in a single call. So a non-terminating query today wedges the
worker permanently and no amount of front-end design fixes that. v1
therefore ships: `cancel` marks the query cancelled so the *front end*
detaches and stays responsive; the engine keeps burning one thread until the
process exits; the status line says so in as many words. When bounded slices
(Phase 3) land, `cancel` becomes real with no API change. **We will not ship
a cancel button that silently lies.**

## 7. Platform and toolchain

- **Language**: C++17, matching `vault-unify-core` exactly (same standard,
  same `-Wall -Wextra` zero-warning bar, same `UNIFY_SANITIZE` option).
- **UI framework**: **FTXUI** (MIT, header+CMake, no ncurses/terminfo
  dependency, vendored via `FetchContent` with a pinned tag). Rationale and
  the rejected alternatives are in [ARCHITECTURE.md](lens/ARCHITECTURE.md)
  §6. The framework is confined behind `lens/src/term/` — see G1's
  acceptance criterion on that isolation, which exists precisely so this
  choice is reversible.
- **Targets**: macOS (Terminal.app, iTerm2), Linux (xterm-family),
  Windows 11 (Windows Terminal + PowerShell, and Git Bash — with the
  caveat in [ARCHITECTURE.md](lens/ARCHITECTURE.md) §6.3, which is a real
  and known-in-advance problem, not a surprise).
- **Build**: `lens/CMakeLists.txt`, a sibling of `unify/CMakeLists.txt`,
  linking `vault-unify-core`. The two stay independently buildable.
- **CI**: extend `.github/workflows/unify-ci.yml` **and its
  `.forgejo/` twin** (README: they must be kept in sync) with a lens build
  + golden-screen job, and add macOS and Windows runners.

## 8. Screen geometry

Probing this session's terminal returned 80×24, which is `tput`'s non-TTY
fallback rather than a real measurement (the tool shell is not a terminal),
so it is treated as evidence of nothing except that 80×24 remains the
floor everyone still hits. The layout engine therefore targets:

- **Hard floor 80×24** — every panel set must remain usable; below this,
  lens prints a message and exits rather than rendering garbage.
- **Default target 120×40** — the geometry all golden screens are recorded
  at, and the size the stock layouts are tuned for.
- **Fluid above that** — panels take fractional weights, not fixed columns.

Both 80×24 and 120×40 are golden-tested for every stock layout (G1).

## 9. Test-driven method

No production line is written before a failing test names it. Three
harnesses, all modelled on the repo's existing golden convention
(`test/run-golden-test.sh`, `UNIFY_UPDATE_GOLDEN=1`):

1. **Session contract suite** (D7) — one suite, parameterised over
   `Session` implementations. It is run against `LocalSession`, against a
   deliberately hostile `FakeSession` (reorders events, delays replies,
   drops a connection), and against `ProxySession` over loopback. *A
   feature is not in the boundary until it passes on all three.* This is
   the mechanism that keeps requirement (c) true rather than aspirational.
2. **Golden screens** (D8) — a key-event script plus a terminal geometry
   in, a rendered character grid out, diffed against a committed file.
   Because rendering is a pure function of the model
   ([ARCHITECTURE.md](lens/ARCHITECTURE.md) §4), these are deterministic
   and fast, with no pty involved.
3. **Engine goldens** — the existing suite, which must stay byte-for-byte
   green throughout; E1–E6 touch shared code paths and the existing
   `.ufy` goldens are the regression net for that.

The ten gates and their acceptance criteria are in
[ACCEPTANCE.md](lens/ACCEPTANCE.md). Every gate additionally carries a
**help criterion** (§9.1) and a zero-warnings/sanitizer-clean criterion.
Summary:

| Gate | Name | Gist of the exit criterion |
| --- | --- | --- |
| G0 | Boundary | Contract suite green on `LocalSession` + `FakeSession`; no engine type in the public session header |
| G1 | Shell | Tiles, resizes, quits; golden screens at 80×24 and 120×40; FTXUI confined to `term/`; **help system live** |
| G2 | Transcript | Define + query + solutions end-to-end against the real engine |
| G3 | Browser | Catalogue and source panels; edit-and-redefine a predicate in place |
| G4 | Image | save / load / insert with all three overwrite policies, round-trip verified |
| G5 | Inspect | Inspector, diagnostics, solutions table; degrades correctly pre-E7 |
| G6 | Debug | Trace panel, breakpoints, goal-stack view |
| G7 | Remote | Contract suite green on `ProxySession`; two-process demo; reconnect resyncs from `seq` |
| G8 | Platform | Green CI on macOS, Linux and Windows 11 |
| G9 | Extension | A third-party module added out-of-tree-in-spirit, touching no lens source file |

### 9.1 Online help is built in from the first gate

Help is not documentation written after the environment works; it is a
subsystem gated at **G1**, before any panel exists that would need
explaining, and every later gate carries "its help exists and is reachable
by `F1`" as an exit criterion. The design is [UI.md](lens/UI.md) §5; the
three properties that make it stay true rather than rot:

- **A command with no help text fails the build.** Help is a required field
  of the command table ([UI.md](lens/UI.md) §6), which is also what
  generates the menus, the hint line and `M-x` — so the four surfaces
  cannot disagree with each other.
- **The language reference is extracted, not restated.** `LANGUAGE.md`
  (~1000 lines, tutorial) and `SPEC.md` (normative) already exist and are
  maintained. A build step turns their reference material into help topics,
  and a test asserts that every builtin the core reports in `describe()`
  has one. Hand-copied help would be wrong within a month.
- **`F1` is contextual and never empty.** It opens the topic for whatever
  has focus — including the builtin name under the cursor in the
  transcript — rather than a table of contents.

## 10. Sequencing

```
E1 E2 E4 E10 E14 ──► D1 D2 ──► G0 ──┐
                                    ├──► G1 ──► G2 ──┬──► G3 ──► G6
                        D9 (help) ──┘                ├──► G5 ◄── E7 E15
E3 E5 E6 E9 E12 E13 ──► D4 D10 (image, journal) ──► G4 ──┘

                        D6 (proxy) ──► G7 ──► G8 ──► G9
```

The critical path runs through the engine, not the UI: **E14** (the
ROADMAP 5.1 concurrency subset) gates G0, and **E9/E12/E13** gate G4. A
schedule that treats lens as a UI project with some engine glue will be
wrong by the width of that column.

G0 is the only hard prerequisite for everything else; G4 (image) and G3/G5
are independent after G2 and can proceed in parallel. G7's proxy work can
start any time after G0, since it is written against the contract suite and
needs no UI at all.

## 11. Explicitly out of scope for v1

Stated so the boundary is not designed for them by accident, and so the
absences are decisions rather than oversights:

- Dynamically loaded (`dlopen`/`LoadLibrary`) plugin modules — C++ ABI
  across a plugin boundary is a trap; v1 modules are statically registered
  ([UI.md](lens/UI.md) §7).
- An extension *language* inside lens (the Emacs Lisp analogue). The natural
  candidate is Unify itself; the command table is designed so that becomes
  additive later.
- Mouse input, overlapping windows, image thumbnails, multi-session
  side-by-side.
- Collaborative/multi-user editing of one remote session. The `seq`-based
  event stream does not preclude it; nothing in v1 pursues it.
- The actual BEAM implementation. This plan makes it *possible* and pays the
  design costs up front; it does not start it.
- TLS for `unify-lensd`. v1 is loopback-by-default plus a shared secret,
  with SSH tunnelling as the documented remote transport
  ([SESSION-API.md](lens/SESSION-API.md) §7).
- An ambitious editor. The editor is scoped deliberately small
  ([UI.md](lens/UI.md) §3.1) and is still the largest single UI item here.
- Undo across a `load`, and undo of query side effects — both marked
  non-undoable rather than silently half-applied ([UI.md](lens/UI.md) §8).

## 12. Review record

The first draft of these documents was reviewed by an independent
architecture pass against the actual sources. It found four issues that
changed the design and a number of factual slips; all are corrected above
and in the companion documents, each marked with a revision note. Recorded
here because the corrections are more useful than the draft was:

**Design-changing:**

1. **The session API was not closed over its own gates.** No trace/debug
   event existed despite G6; no way to expand a `truncated` value despite
   G5; no query-lifetime verb, so demand-driven delivery leaked retained
   queries by construction. Fixed by `inspect`, `release`, `debug` and
   `TraceEvent`/`Expanded` — 15 requests and 12 events, up from 13 and 7.
2. **`Output` and `Diagnostic` had no query attribution.** Harmless today
   (one FIFO worker serialises everything), unfixable-without-a-break the
   moment a BEAM core runs two queries at once — which is the entire point
   of the boundary. `QueryId` and `querySeq` now sit on the event header.
3. **The image format contradicted the environment's central gesture.**
   Verbatim module text goes stale the instant a predicate is redefined in
   place, retraction of a module-defined clause was unrepresentable, and
   clause order — which is trial order in this engine — did not survive.
   One mechanism fixed all three: an ordered `@changes` log, which also
   supplies the crash journal the draft lacked.
4. **Two of the largest engine obligations were absent from the E-list.**
   World replacement for `load` (E9) and the concurrency safety that
   dropping `waitForEngineIdle()` demands (E14).

**Factual corrections:** the "13 requests / 7 events" and "nine gates"
counts were both wrong by their own contents; `unify-repl.cpp:487` is
actually `:489`; G0.2's no-raw-pointers criterion contradicted
`subscribe(EventSink*)`; structured diagnostics were described as existing
when the engine only produces formatted stderr text and an error count; and
the claim that atomic `insert` was "implementable on the current engine" by
staging into an `ExecutionState` was wrong on three counts
([IMAGE-FORMAT.md](lens/IMAGE-FORMAT.md) §5.1).

**Reasoning corrected:** the claim that BEAM's pairwise-only message
ordering *implies* a single ordered stream was backwards — a global order
across concurrent processes requires a serialising funnel, the very shape
the same argument warned against. The single stream is a front-end
simplicity choice and is now defended as one.

**Omissions now specified:** the text editor (a table row, now §3.1 of
[UI.md](lens/UI.md) and the largest UI item in the plan), undo of world
operations, `unify-lensd`'s trust boundary, crash recovery, transcript
persistence and export, completion, and grapheme-cluster width.

Two review points are acknowledged and **not** fully resolved, deliberately:
character-grid goldens remain part of the harness (demoted to a layout
check, with model-level assertions primary — ACCEPTANCE rule 6), and the
help-extraction step is a parser that can drift, so its test proves topic
presence rather than correctness and the extractor fails loudly on
unrecognised input.
