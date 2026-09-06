# `unify-lens` — the screen

Companion to [`../lens-text-mode-environment.md`](../lens-text-mode-environment.md).
Settles plan requirements (a), (d) and (f): the text-mode environment
itself, its tiling, its help, and how other IDE modules extend it.

> **Revision note (2026-09-06, after architecture review).** The first draft
> disposed of the text editor in one table row and had no undo of world
> operations. Both are now sized and specified (§3.1, §9): the editor is the
> single largest UI item in the plan and the surface users touch most, and
> an environment whose flagship gesture destroys clauses without an undo
> will burn people. §10 adds the smaller omissions the review found —
> concurrent queries, transcript persistence, completion, export, and
> character width.

---

## 1. The frame

Four fixed regions, Borland-shaped, at every geometry:

```
row 0          menu bar          File  Edit  World  Query  Image  Debug  Window  Help
rows 1..h-3    tile area         the panels (section 3)
row h-2        status line       world gen · clauses · query state · session · layout
row h-1        hint line         F1 Help  F2 Save  F3 Insert  F5 Run  F9 Trace  F10 Menu
```

The menu bar is reachable with `F10` (Borland) or `Alt-<letter>`; the hint
line always shows the *currently applicable* function keys, which is the
single feature that made those IDEs learnable without documentation. It is
generated from the command table (§6), so a module's commands appear there
without lens knowing about them.

Everything else is tiles. There are no overlapping windows and no floating
palettes; dialogs are tiles too (§4).

## 2. Tiling

A binary tree of splits with fractional weights, in the Oberon spirit:
non-overlapping, resizable, and always covering the tile area exactly.

- `Ctrl-x 2` / `Ctrl-x 3` split horizontally / vertically (Emacs bindings,
  because they are the ones with muscle memory behind them; fully
  rebindable).
- `Ctrl-x 0` close, `Ctrl-x 1` maximise, `Tab` / `Shift-Tab` cycle focus.
- `Ctrl-x b` switches which *buffer* a tile shows. Buffers and tiles are
  decoupled exactly as in Emacs: any panel can be shown in any tile, and
  the same buffer can be open in two tiles.
- Minimum tile size is 20×5; the solver drops the least-recently-focused
  tile to a one-line title stub rather than rendering a tile too small to
  be read.

### 2.1 Stock layouts

Recorded as goldens at both 80×24 and 120×40.

**`browse` (default, 120×40)** — the Smalltalk system-browser stance:

```
┌ Catalogue ─────────┬ Source: play/2 ───────────────────────────────────┐
│ ▾ mediaplayer      │ play( $room, $track ) {                           │
│     play/2       4 │     room( $room );                                │
│     room/1       3 │     device( $room, $dev );                        │
│     volume/2     2 │     send( $dev, "play", $track );                 │
│   ▸ __synth (6)    │ }                                                 │
│ ▾ <transcript>     │                                                   │
│     scratch/1    1 ├ Diagnostics ──────────────────────────────────────┤
│ ▸ builtins (37)    │ (none)                                            │
├────────────────────┴───────────────────────────────────────────────────┤
│ Transcript                                                             │
│ ?- play(hall, "nocturne").                                             │
│    yes.  $dev = amp_hall                                               │
│ ?- █                                                                   │
└────────────────────────────────────────────────────────────────────────┘
 gen 41 · 12 clauses · 4 preds · idle · local · browse
 F1 Help  F2 Save  F3 Insert  F5 Run  F6 Inspect  F9 Trace  F10 Menu
```

**`run`** — transcript maximised, solutions table beside it, catalogue as a
stub. The stance for driving a program.

**`debug`** — goal stack, trace, source with the current goal highlighted,
bindings inspector. The stance for finding out why it does that.

**`full`** — everything at once; only sensible above ~150 columns, and
present because someone will have that screen.

At 80×24 each preset degrades to a two-tile arrangement plus stubs, with
transcript always visible. Below 80×24 lens exits with a message rather
than rendering something illegible.

## 3. The ten v1 panels

Each is an IDE module (§7); none is privileged in the code.

| Panel | Shows | Talks to the core via |
| --- | --- | --- |
| **Transcript** | The REPL: input history, echoed goals, results, `Output` events. Past input is re-runnable in place (Oberon: text is executable) — put the cursor on an old line and `F5`. | `define`, `solve` |
| **Catalogue** | module → `(name, arity)` → clause count, with builtins and synthesized clauses collapsed into counted stubs. Filter-as-you-type. | `listing`, `WorldChanged` |
| **Source** | The selected definition's text, in the editor of §3.1. `F3` redefines it in place under `ReplacePredicates`; `Ctrl-z` undoes the *world* change, not just the text (§8). | `source`, `define` |
| **Solutions** | The current query's bindings as a table, one row per solution. Scrolling past the last row issues `demand` — the back-pressure of [ARCHITECTURE.md](ARCHITECTURE.md) §5 made visible. | `demand`, `cancel` |
| **Inspector** | The selected binding as an expandable term tree, `truncated` nodes marked and expandable on request. | (renders `Value`) |
| **Diagnostics** | `Diagnostic` events as a navigable list; Enter jumps the source panel to file/line/column. | (renders events) |
| **Trace** | Goal stack, breakpoints, step controls. Renders "unavailable" when `describe()` reports no debug capability. | `setTrace`, `breakpoint` |
| **Image** | Current image path, startup goal, module list with digest-drift markers, and the save/load/insert actions with their policy selector. | `save`, `load`, `insert` |
| **Queries** | Every query in flight or retained: id, goal, state, solutions produced, and whether it still holds engine memory. `release` and `cancel` act from here. The API permits concurrent queries; without this panel a retained query is invisible and a leak is undiagnosable. | `cancel`, `release` |
| **Help** | §5 — always available, never absent. | — |

### 3.1 The editor — the largest single item in this plan

"Editable" was one word in the first draft and is person-months of work. It
is the surface a user of this environment touches more than any other, so
it is scoped explicitly rather than assumed.

**v1 scope** — a competent single-file text editor, no more:

- Multi-line editing with a gap buffer; movement by character, word, line,
  paragraph, buffer; selection.
- **Undo/redo of text**, unbounded within a session, grouped by
  edit-burst — separate from world undo (§8), which it composes with.
- Kill-ring (`Ctrl-k`/`Ctrl-y`) and system-clipboard bridging where the
  terminal supports OSC 52; a plain "write region to file" everywhere else.
- Auto-indent that matches `{ }` nesting, and bracket matching — Unify's
  surface syntax is C-like, so this is cheap and worth a lot.
- Syntax highlighting from one small tokenizer shared with the transcript:
  keywords, `$vars`, strings, comments, numbers. Degrades to attributes in
  the monochrome tier.
- Correct **grapheme-cluster width** handling: `wcwidth`-class measurement
  for CJK and combining marks, because a wide character in a clause head
  otherwise misaligns every column in every panel — and every golden.
- **Completion** (`Tab`): predicate names from the catalogue, builtins from
  `describe()`, and variables in scope in the buffer.

**Explicitly not v1**: multiple cursors, rectangular selection, macros,
regex search-and-replace across the world, LSP-anything, and any attempt to
be Emacs. A modest editor that never loses text beats an ambitious one.

**Gating**: the editor gets its own gate criteria under G3 (see
[ACCEPTANCE.md](ACCEPTANCE.md)) — a property test that random edit
sequences never corrupt the buffer, and a width test over a CJK and
combining-mark corpus.

## 4. Dialogs and the minibuffer

- The **minibuffer** occupies the hint line when active: `M-x` opens the
  command palette over the whole command table (§6), with completion and a
  one-line description per command. Every command is reachable here, bound
  or not.
- **Dialogs** (open file, save image, confirm destructive load) are
  temporary tiles that take focus, not overlays. They obey the same tiling
  solver, so they cannot land off-screen or clip at 80×24 — a class of bug
  that simply does not arise.
- `Esc` always cancels the innermost thing; `Ctrl-g` aborts to the
  transcript from anywhere.

## 5. Online help — present from the first gate

Help is built in from the start, not documented later. It is gated at **G1**
(the shell gate), before any panel that has features to explain, and every
subsequent gate carries "its help exists and is reachable" as an exit
criterion. Concretely, four layers:

### 5.1 The hint line (always)

Row `h-1` always shows the function keys applicable to the *focused* panel,
derived from the command table. It is never blank and never stale — a
module that registers a command with a function-key binding gets a hint for
free, and one that does not register help text fails the module test (§7.2).

### 5.2 `F1` — contextual help (always)

`F1` opens the Help panel on the topic for whatever has focus: the focused
panel's own page, or, in the transcript, the topic for the word under the
cursor — a builtin name, a keyword, a `(name, arity)`. `F1` never does
nothing and never opens a table of contents when it could open the answer.

### 5.3 The Help panel

A tile like any other, so help sits *beside* the work rather than covering
it — which is the whole argument for tiling. It renders a small hypertext:
topics, `[[links]]`, `Backspace` for back, `/` to search full text. Its
content comes from three sources, merged:

| Source | Content | Kept true by |
| --- | --- | --- |
| **Built-in topics** | Compiled-in pages: getting started, the tiling model, each panel, images and the three verbs, the keymap reference | Golden-screen tests render them |
| **Command table** | Auto-generated page listing every registered command, its description and its current binding | Generated at runtime from the live table, so it cannot disagree with the bindings |
| **Language reference** | Builtin/keyword topics extracted at build time from `unify/LANGUAGE.md`'s reference appendix and `SPEC.md` section headings into `lens/help/generated/` | A build step, plus a test that every builtin `describe()` reports has a topic. Note the honest limit: the extractor is itself a parser and can drift from the documents' formatting; the test proves *presence*, not correctness, and the extractor fails loudly on an unrecognised heading rather than silently emitting nothing |

The third row is the important one: the language documentation already
exists and is maintained (`LANGUAGE.md`, ~1000 lines; `SPEC.md`, normative).
Lens **extracts** from it rather than restating it, so `F1` on `findall`
shows what the spec says and cannot fall behind it. A hand-copied help text
would be wrong within a month.

### 5.4 `:help` and `M-x`

The transcript accepts `:help [topic]` (continuous with the existing REPL's
`:help`, so nobody has to unlearn it), and `M-x` shows every command with
its description inline — so the command palette is itself a help surface.

### 5.5 First-run guidance

An empty world on first start shows, in the transcript, four lines: what
this is, `F1` for help, `F10` for the menu, and how to load a file. Not a
tutorial, not a splash screen, and dismissed permanently by any keystroke.

## 6. Commands, keymaps and the hint line

One table underlies all four help surfaces and every binding:

```cpp
struct Command {
    std::string id;            // "world.insert-file", "query.cancel"
    std::string title;         // "Insert file…"
    std::string help;          // one paragraph, shown in M-x and F1
    std::string helpTopic;     // optional link into the help hypertext
    Predicate   enabled;       // (const Model&) -> bool
    Handler     run;           // (Model&) -> std::vector<Request>
};
```

Keymaps bind **command ids**, never functions, and live in config
(`lens.toml`), so rebinding needs no rebuild. Menus, the hint line, `M-x`
and the generated help page are four renderings of this one table. A
command with no `help` text is a build-time failure, which is the mechanism
that keeps §5 true as the environment grows.

`Handler` returns requests rather than issuing them, per
[ARCHITECTURE.md](ARCHITECTURE.md) §4 — so "what does this key do?" is an
assertable value in a test.

## 7. Extensibility — other IDE modules (requirement (f))

### 7.1 The interface

```cpp
class Module {
public:
    virtual ~Module() = default;
    virtual ModuleInfo  info() const = 0;              // id, title, help topic
    virtual void        registerCommands( CommandTable& ) = 0;
    virtual PanelPtr    createPanel( PanelKind ) = 0;
    virtual void        onEvent( const SessionEvent&, ModelWriter& ) = 0;
    virtual std::vector<HelpTopic> helpTopics() const = 0;
};
```

A panel implements the same pure pair as everything else:

```cpp
    Fold  fold( PanelState&, const Event& ) -> std::vector<Request>;
    void  view( const PanelState&, Canvas& ) const;
```

Registration is static, at link time, via a self-registering translation
unit. Adding a module means adding a directory and one line to
`lens/CMakeLists.txt` — and touching **no existing lens source file**. That
"no existing file" property is the actual extensibility claim, and gate G9
tests it as such, by adding a real module and diffing the tree.

Dynamic (`dlopen`) plugins are out of scope for v1: a C++ ABI across a
plugin boundary is a maintenance trap, and the interface above is
deliberately not designed for one. The forward path, if it is ever wanted,
is a module implemented *in Unify* driving the command table through the
session — which is additive and is why the command table is data.

### 7.2 What a module gets, and owes

**Gets**, without lens changing: menu entries, hint-line keys, `M-x`
entries, config-file rebinding, help pages, tiles, session events, and the
ability to issue any of the 15 requests.

**Owes**: help text for every command, at least one help topic, a golden
screen at 80×24 and 120×40, and purity — a module that performs I/O in
`view` or blocks in `fold` fails the module conformance test.

### 7.3 Modules the design anticipates

Not built in v1; listed because they are the check on whether the interface
is wide enough. A profiler panel (needs a new event kind — the honest test
case for extending the boundary), a test-runner panel over the golden
corpus, a graph view of the predicate call graph, a diff panel for image
versus source drift, and a notebook-style panel that keeps queries and
their outputs as re-runnable cells.

## 8. Undo of world operations

The environment's flagship gesture — redefine a predicate in place under
`ReplacePredicates` — destroys clauses. A confirmation dialog is not a
substitute for undo, and this engine makes undo unusually cheap: clauses
are tombstoned rather than removed (`Clause::retire()`), and every mutation
is stamped with a world generation.

So lens keeps a bounded **world undo stack** of inverse operations, built
from the same `@changes` entries the journal already records
([IMAGE-FORMAT.md](IMAGE-FORMAT.md) §3):

- `Ctrl-z` in the source panel, or `M-x world.undo`, reverts the last
  world mutation — re-asserting retracted clauses in their original order,
  retracting newly added ones.
- The Image panel shows the undo stack as a change list, so "what did I do
  to this world?" is answerable, which is half of why the journal exists.
- **Bounded and honest**: side effects of queries (`emit`, an actuator
  call) are *not* undoable, and the stack marks any entry that spans one.
  Undo that silently fails to un-ring a bell is worse than no undo.
- Not undoable across a `load`. `load` is the one bright line, which is why
  it confirms.

## 9. Smaller things a real environment needs

Each is small; each is the kind of omission that makes an otherwise good
tool feel unfinished:

- **Transcript persistence.** The existing REPL keeps `$HOME/.unify_history`
  (`unify-repl.cpp:143`). Lens keeps the same file, in the same format, so
  history carries across the two front ends rather than being lost by the
  upgrade.
- **Export.** `M-x transcript.write-file`, and `M-x solutions.export-csv`
  from the solutions table. A TUI with no mouse and no way to get text out
  is a dead end; this is the way out.
- **Search.** Incremental search within a panel (`Ctrl-s`), and
  `M-x world.grep` over clause source across the catalogue.
- **Character width**, as §3.1 — it belongs to the whole `CellGrid`, not
  only to the editor.
- **A dirty indicator** on the source panel and in the status line, so an
  unsubmitted edit is never mistaken for a redefined predicate.

## 10. Interaction principles

Stated as rules because they are what makes the four influences one
environment instead of four:

1. **Nothing blocks.** No spinner, no modal wait. A running query shows in
   the status line while the rest of the environment stays live.
2. **Nothing is hidden.** Every state change that alters the world is
   reported in the transcript, including ones made from panels. The
   transcript is the log of the session, not just the prompt.
3. **Destructive things are named.** `load` and `ReplaceModule` confirm and
   say what will be lost. `Defined` events are reported as
   `route/3 replaced (4 clauses)`, never silently.
4. **Colour is never the only signal.** See [ARCHITECTURE.md](ARCHITECTURE.md) §6.2.
5. **The screen at 80×24 is a supported product**, not a degraded
   afterthought — it is golden-tested for every layout.
6. **`F1` always answers**, and answers about what is focused (§5.2).
