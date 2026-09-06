# `unify-lens` — Architecture

Companion to [`../lens-text-mode-environment.md`](../lens-text-mode-environment.md).
This document settles structure, threading and data flow. The boundary
itself is specified in [SESSION-API.md](SESSION-API.md); the screen in
[UI.md](UI.md).

> **Revision note (2026-09-06, after architecture review).** Two corrections
> and one addition: the BEAM-ordering justification in §2.3 was backwards
> (§2.3, corrected); §3.1 is new and names the concurrency obligation that
> dropping `waitForEngineIdle()` creates — the review's sharpest point, and
> the reason a subset of ROADMAP 5.1 is now in scope rather than adjacent
> to it; §8 states the daemon's trust boundary.

---

## 1. Layered structure

```
 lens/src/
   app/        composition root: parse argv, build a Session, run the loop
   model/      Model, Event, Command  -- pure, no I/O, no framework types
   panels/     the nine v1 IDE modules (see UI.md section 3)
   modreg/     module registry, command table, keymap resolution
   layout/     tiling tree, geometry solver, layout presets
   term/       FTXUI containment: Screen -> cell grid, key decoding, resize
   session/    ProxySession + transports (LocalSession lives in the engine)
   test/       golden-screen harness, key-script driver
```

The dependency rule is one arrow: `app → panels → model`, and
`app → term`. **`model/` and `panels/` include no FTXUI header and no
engine header.** They see the session boundary types and their own types
and nothing else. This is what makes both the golden-screen harness and the
framework swap possible, and it is checked mechanically (G1).

## 2. Three shapes of core, one interface

`unify::session::Session` is a pure interface. Three implementations are
foreseen; the differences between them are exactly the constraints the
interface was designed around.

### 2.1 `LocalSession` — in-process

Owns a `RuntimeContext`, an engine worker thread, and an event queue.
It *could* answer synchronously — it deliberately does not. Requests are
posted, and every answer, diagnostic and solution arrives on the same
ordered event queue that a socket would deliver. The extra indirection is
the price of the abstraction being real rather than nominal, and it is
what lets the same contract suite run against all three.

`LocalSession` is the only place in the tree that knows about `Clause*`,
`UnifyContext*`, `SolveJob` or `WorldPtr`.

### 2.2 `ProxySession` — remote

Serialises requests, deserialises events, over a transport. Adds three
things a local session never sees, all of which the interface must already
express: **latency** (so no request may be blocking), **partial failure**
(so every request can be answered by a `Failed` event, and every long
operation is cancellable in principle), and **reconnection** (so events are
sequenced and the world is generation-stamped).

`unify-lensd` is the other end: a thin head that owns a `LocalSession` and
speaks the transport. It is deliberately not a "server product" — it is
`LocalSession` with a socket in front, so that the remote path is exercised
by the same code the local path uses.

### 2.3 `BeamSession` — the reason for the discipline

Not built here. Its constraints are what shaped the interface:

| BEAM property | Consequence already baked into the API |
| --- | --- |
| No shared memory; messages are **copied** | Values are self-contained trees. No handle is ever a pointer; ids are opaque integers. Nothing the front end holds can dangle. |
| A query is a **process**; mailboxes are unbounded | Solutions are **demand-driven** (`demand(n)`). A producer faster than the consumer must never be allowed to accumulate. |
| Processes **crash and are restarted** by a supervisor | Every request may be answered `Failed{reason}`; the front end must treat a query dying as normal, not exceptional. A `QueryStatus::Aborted` is a first-class outcome. |
| Ordering is guaranteed only **pairwise** between two processes | Every event carries a per-query `querySeq` alongside the global `seq`, so a front end can depend on per-query order alone. The *global* order is a front-end simplicity choice, not something BEAM implies — see [SESSION-API.md](SESSION-API.md) §5, which corrects the first draft's reasoning here. |
| stdout belongs to the node, not the caller | `print`/`emit` are **events** (engine item E4), never process output — and carry the `QueryId` that produced them, because two concurrent query processes interleave their output and unattributed text is unreadable. |
| A query process **holds its own term memory** | Solutions stay retained until `release`, so `inspect` can expand a truncated value without re-running a nondeterministic goal. |
| The world may live in a process or ETS, versioned | `worldGeneration` on every event; views know when they are stale. |

The test of whether this discipline held is mechanical: `FakeSession` in
the contract suite (D7) deliberately reorders independent replies, delays
them, injects `Failed`, and drops the connection mid-query. Front-end code
that assumed in-process behaviour fails there before it can reach a user.

## 3. Threading

Three threads, and one rule.

| Thread | Owns | May touch |
| --- | --- | --- |
| **UI thread** | `Model`, all panels, the terminal | Nothing else. Never blocks. |
| **Session thread** | the transport or the local engine bridge | Its own queues |
| **Engine worker** | `Engine::executionLoop()`, unchanged | Engine internals only |

The rule: **the UI thread never blocks on the session, and the session
thread never touches the model.** They communicate through two lock-free-in-
spirit queues (a mutex + condvar is fine; the volumes are keystroke-scale):
requests out, events in. The UI loop wakes on terminal input *or* on an
event arriving, folds it into the model, and redraws.

This is the direct replacement for `waitForEngineIdle()`
(`unify/tools/unify-tool-support.hpp`), whose barrier-job trick depends on
a single FIFO worker in the same address space — true today, false in every
direction this plan points.

### 3.1 Dropping the idle barrier has a price, and it is ROADMAP 5.1

`waitForEngineIdle()` is not merely a convenience; it is the discipline the
engine's current thread-safety argument rests on.
`World::clauseDbMutex()`'s comment (`vault-unify.hpp`) is explicit that the
**reader** side is unlocked because "this engine has exactly one reader
today, the single worker thread". The REPL preserves that by waiting for
idle after every input, so a parse never overlaps a solve.

A session that never blocks breaks it: a `listing` read, or a `define`
parse — which writes `World::setTermDebugInfo` (unlocked) and bumps
unsynchronized static id counters — concurrent with a running query is
exactly the race ROADMAP 5.1 exists to fix.

Two options, and the plan takes the second:

1. **Serialise inside `LocalSession`** — queue every request behind the
   running query. Cheap, and unacceptable: a runaway query would wedge not
   just its own thread but every subsequent `define`, `listing`, `source`
   and `save` in the session. That is strictly worse than the honest cancel
   limitation in the plan's §6, and it would make the environment
   unusable in precisely the situation an environment exists for.
2. **Do the 5.1 subset first** — engine item **E14**: atomic id counters,
   a locked `setTermDebugInfo`, and a reader-side lock or generation
   snapshot for catalogue reads. This is a bounded, already-specified piece
   of ROADMAP 5.1, not new research, and it is a prerequisite for G0 rather
   than a later cleanup.

Interim, until E14 lands, `LocalSession` serialises **writes** against the
worker and permits concurrent catalogue reads only from the E2 catalogue
index (which the session owns and maintains from `Defined`/`WorldChanged`,
not by walking the live clause list). Gate G0 tests "define and listing
while a query runs" precisely so this cannot be quietly skipped.

## 4. The model is a pure state machine

```
    fold : (Model, Event) -> (Model, [Request])
    view : (Model, Geometry) -> CellGrid
```

`Event` is the union of terminal events (key, resize, paste) and session
events. `fold` is total, allocation-cheap, and free of I/O. `view` is a
pure function — the same model and geometry always render the same grid.

Everything the environment can do therefore has a deterministic,
reproducible test that needs no terminal, no pty and no timing
(harness D8). This is not a testing convenience bolted on afterwards; it is
the reason the layering in §1 is worth enforcing.

**Assert on the model first, the grid second.** A rendered character grid
embeds content — generation counters, clause counts, solution ordering — so
using grids as the primary behavioural oracle turns every cosmetic tweak
into a dozen re-recorded goldens: the classic screenshot-test trap. The
`fold`/`view` split exists so behaviour can be asserted on the *model*
(`fold` produced these requests, the model holds these rows) and grids
reserved for what only they can catch: layout, degradation at 80×24, and
that a panel renders at all. [ACCEPTANCE.md](ACCEPTANCE.md) states this as
a rule of the method.

Panels hold their own sub-state inside `Model` and expose the same two
functions over it. A panel that wants to talk to the engine returns a
`Request` from its `fold`; it never calls the session directly. That keeps
"what did this keystroke ask the engine to do?" an assertable value.

## 5. Data flow of one interaction

Typing `?- route(hall, kitchen, $p).` in the transcript:

```
 key events ──► term/ decode ──► Event::Key
                                    │
                                    ▼
                          fold(Model, Key) ──► Model'   (echo into transcript)
                                    │
                                    └─► Request::Solve{ text, limit: 20 }
                                            │
        UI thread ──────────────────────────┼──── request queue ────┐
                                            │                       ▼
                                            │              Session thread
                                            │                       │
                                            │              LocalSession posts a
                                            │              SolveJob / Proxy sends
                                            │                       │
                                    event queue ◄───────────────────┘
                                            │
             Event::Solution{ qid, seq, bindings } (up to 20)
             Event::Output{ ... }             from print/emit  (E4)
             Event::Diagnostic{ ... }
             Event::QueryStatus{ qid, Complete | Exhausted | Failed | Aborted }
                                            │
                                            ▼
                              fold ──► Model'' ──► view ──► redraw
```

Panel updates fan out from one folded event: the transcript appends, the
solutions table gains rows, the inspector rebinds to the selected row, the
status line updates counts. No panel polls, and no panel has its own
connection to the engine.

If the user scrolls past row 20, the solutions table's `fold` emits
`Request::Demand{ qid, 20 }`. That is the entire back-pressure design, and
it is the same code locally and remotely.

## 6. Terminal layer

### 6.1 Choice: FTXUI

**FTXUI** (MIT, C++17, CMake `FetchContent`, pinned tag). It brings its own
terminal handling and needs neither ncurses nor terminfo, which is what
makes one CMake file work on all three platforms. Its own layout system is
**not** used — lens owns tiling (§1, `layout/`) — so FTXUI is used as a
screen buffer, an input decoder and a resize signal, roughly a tenth of its
surface.

Rejected, with reasons:

- **ncurses** — the obvious choice, and the wrong one here: PDCurses on
  Windows is a separate implementation with its own divergences, and the
  terminfo dependency is a packaging tax on macOS and Windows both.
- **notcurses** — more capable (and genuinely better at Unicode), but a
  heavier dependency and weaker Windows support than FTXUI.
- **Hand-rolled ANSI** — attractive because the needs are small, and it is
  the fallback if FTXUI disappoints. Rejected as the *starting* point only
  because Windows console mode setup and key decoding are precisely the
  fiddly, platform-divergent parts that a library earns its place on.

Containment is the mitigation and it is testable: FTXUI appears only under
`lens/src/term/`, behind `ITerminal { draw(CellGrid); poll() -> Event; }`.
G1 gates on a grep proving that. Replacing it is then a two-file job.

### 6.2 Colour and capability

Three tiers, auto-detected, overridable by flag: truecolour, 256-colour,
and **monochrome + attributes**. Every panel must be legible and every
selection visible in the monochrome tier — no meaning is ever carried by
colour alone. Golden screens record the character grid, so they are
colour-independent by construction and this cannot silently regress.

### 6.3 Windows 11, and the Git Bash problem — stated up front

- **Windows Terminal / PowerShell / cmd**: the supported path. Virtual
  terminal processing is enabled at startup; this is the configuration CI
  tests.
- **Git Bash (mintty)**: mintty is not a Win32 console — it is a pty
  frontend, and native console applications running under it get no real
  console handle. This is the same reason `python`, `node` and `winpty`
  exist as a workaround culture there. Expect a plain-terminal-mode
  degradation or a `winpty unify-lens` invocation, **not** the full
  environment.

  The plan's position: **support it explicitly as a degraded mode.** At
  startup lens detects "no real console" and, rather than drawing a broken
  screen, prints one line naming the problem, the `winpty` workaround, and
  the option to fall back to `unify-run -i`. A user hitting this must be
  told why in one sentence, not left with a corrupted terminal.

  G8 tests Windows Terminal as the *supported* target and pins the Git Bash
  message as a golden. Promising more than that would be dishonest about a
  known platform limitation, and the user asked for Git Bash by name — so
  it gets a defined, tested behaviour rather than silence.

## 7. Startup and configuration

```
unify-lens [options] [program.ufy | image.ufyimg]
  --connect HOST:PORT     use ProxySession instead of an in-process engine
  --layout NAME           start in a named layout (default: browse)
  --geometry COLSxROWS    force geometry (golden-screen harness; also useful
                          for reproducing a bug report)
  --script FILE           run a key-event script, dump the final screen, exit
  --no-color / --color=…  override capability detection
  --config FILE           default: $XDG_CONFIG_HOME/unify/lens.toml,
                          %APPDATA%\unify\lens.toml on Windows
```

Configuration covers keybindings (command-name → key, so a rebinding never
needs a recompile), layout presets, colours and the default image path. It
is read once at startup; there is no live reload in v1.

`--script` is not only a test hook: it makes any lens bug reproducible by a
file, which matters for a program whose bugs are otherwise described as
"the screen looked wrong".

## 8. `unify-lensd` trust boundary

Stated here because it is an architectural property, not a feature: the
daemon accepts `define`, which can `import` any file it can read and, in
the vault lineage this engine comes from, eventually drive actuators. It
therefore binds to loopback by default, requires an explicit `--listen`
plus a shared-secret token for any other bind, and documents SSH tunnelling
as the supported remote transport. TLS is out of scope for v1. The rules
are specified in [SESSION-API.md](SESSION-API.md) §7 and gated by G7.
