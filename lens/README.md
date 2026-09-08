# unify-lens

A text-mode operating environment for Unify: a tiling, keyboard-driven
environment in the lineage of the Borland IDEs, Oberon and the Smalltalk
system browser, in which a Unify world is something you inhabit rather than
something you invoke.

| Document | What it is |
| --- | --- |
| [`../plans/todo/lens/`](../plans/todo/lens/) | The plan, its acceptance gates, and the design documents this implements. Start at [`HANDOFF.md`](../plans/todo/lens/HANDOFF.md). |
| `README.md` (this file) | How to build, test and run it. |

This module is under construction. See **Status** below for what actually
works today; the short version is that the shell exists, no panel does yet.

---

## Requirements

- **CMake** ≥ 3.16
- **A C++17 compiler.** GCC and Clang on Linux and macOS; MSVC or MinGW-w64
  on Windows (see [Windows 11, with vcpkg](#windows-11-with-vcpkg)).
- **Boost** — inherited from the engine, which lens builds as a subproject.
  See [`../unify/README.md`](../unify/README.md). On Windows this comes from
  vcpkg via `VCPKG_ROOT`.
- **bash** — for the golden-screen and layering harnesses. CMake skips
  those tests with a warning if it is missing; the C++ tests still run. Git
  Bash counts.
- **Network access at configure time**, once, to fetch FTXUI — unless you
  build with `-DLENS_BUILD_TERM=OFF` (see below).

## Building

```sh
cmake -S lens -B build/lens -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/lens --parallel
ctest --test-dir build/lens --output-on-failure
```

lens builds the engine as a subproject, so this one command builds both and
`ctest` runs both suites. There is no top-level CMake file in this
repository; each module builds on its own, the way `unify/` does.

### Windows 11, with vcpkg

> **Untested.** CI runs on Linux only, and everything below was written for
> Windows rather than verified on it. Treat it as a starting point, not as a
> recipe known to work — and please correct this section when you find out
> what actually happens.

Boost comes from vcpkg. `VCPKG_ROOT` is not read by CMake on its own, so
point `CMAKE_TOOLCHAIN_FILE` at it explicitly:

```sh
"$VCPKG_ROOT/vcpkg" install boost-spirit boost-thread boost-filesystem

cmake -S lens -B build/lens \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/lens --config RelWithDebInfo --parallel
ctest --test-dir build/lens -C RelWithDebInfo --output-on-failure
```

This works from Git Bash, PowerShell or `cmd`; only the quoting changes. In
Git Bash a Windows-style `VCPKG_ROOT` (`C:\vcpkg`) is fine — CMake accepts
mixed separators — but quote the variable, because a path containing
`Program Files` will otherwise split.

Check `VCPKG_ROOT` is actually set *in the shell you are using*: a Windows
environment variable does not necessarily reach Git Bash, and if it is empty
the argument becomes `/scripts/buildsystems/vcpkg.cmake`, which Git Bash
rewrites to the Git installation prefix. The error then names a path under
`…/Programs/Git/scripts/…` and says nothing about the variable being unset.
A Windows path with forward slashes sidesteps the translation entirely.

vcpkg's Boost is modular, so if a header turns up missing, install the
matching `boost-<lib>` port rather than reaching for anything larger. The
sledgehammer, `vcpkg install boost`, works and takes a long time.

Three things specific to this codebase on Windows:

- **Visual Studio installed without its C++ half.** VS 2026 can be present
  and still have no `cl`, no `nmake` and no `vcvarsall.bat` — they all come
  from the *Desktop development with C++* workload, which an upgrade from an
  earlier VS does not necessarily carry over. The symptoms name none of
  this: vcpkg says "Unable to find a valid Visual Studio instance", and
  CMake silently falls back to the NMake generator and then reports that
  `nmake` does not exist. Check with
  `vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`;
  an empty answer means the workload is missing. Note that `which cl` in Git
  Bash proves nothing — `cl` is only ever on PATH inside a Developer Command
  Prompt.

  **Delete the build directory after fixing it.** CMake caches the generator
  and the toolchain file, so a directory configured during the broken state
  keeps failing the same way and the fix appears not to work.

- **`UNIFY_BUILD_XDEBUG` already defaults to `OFF`** off UNIX. The xdebug TCP
  backend has a POSIX `::access()` call, so it is excluded rather than
  patched. Nothing else in the engine needs it.
- **`/utf-8` and `/bigobj` are set for you.** Both CMakeLists pass them
  under MSVC, so neither is something you need to remember. `/utf-8` because
  the sources and their literals are UTF-8 and MSVC otherwise reads them in
  the system codepage — which turns a box-drawing character constant into
  `error C2015: too many characters in constant` and silently corrupts
  non-ASCII string literals that *do* compile. `/bigobj` because Boost.Spirit
  Qi generates enough template instantiations to exceed MSVC's default
  per-object section limit (`C1128`).

- **vcpkg's Boost is modular, and a missing port looks like a broken
  source file.** `boost-format` is only needed for the xdebug backend, which
  is off by default away from UNIX. If a `boost/*.hpp` genuinely cannot be
  found, install the matching `boost-<lib>` port rather than assuming the
  include is wrong — but check first that the include is not simply
  unnecessary, which is what two of them turned out to be.

The same toolchain file builds the engine on its own, if that is all you
want: `cmake -S unify -B build/unify -DCMAKE_TOOLCHAIN_FILE=…`.

#### MSYS2, if you would rather have GCC

Git Bash ships no compiler — it is a cut-down MSYS2 with no `pacman` — so a
GCC build means installing MSYS2 separately and using **its** MINGW64 shell:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-boost

cmake -S lens -B build/lens -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/lens --parallel
```

No vcpkg, no `/bigobj` question, and a `bash` the test harnesses can use.

#### Running it, which is the part that will not work under Git Bash

Building from Git Bash is fine. *Running* `unify-lens` there is not, and this
was known in advance — [`ARCHITECTURE.md`](../plans/todo/lens/ARCHITECTURE.md)
§6.3: mintty is a pty front end, not a Win32 console, so a native console
application gets no console handle. lens detects that and prints one line
naming the problem and the `winpty` workaround rather than drawing a broken
screen.

**Windows Terminal, PowerShell and `cmd` are the supported path.** Under Git
Bash, use `winpty unify-lens`, or `unify-run -i` for the plain REPL, or
`--script` for anything headless.

One caveat, and it is a weakness in lens rather than in Windows: that
detection currently uses `_isatty`, which is very likely the *wrong* test
here. Under `winpty` the standard streams are proxied through pipes, so
`_isatty` may report false even though a real console exists — meaning lens
could refuse to start in exactly the situation `winpty` was meant to rescue.
The right test is `GetConsoleMode` on the actual handle. Nobody has run it
either way yet.

### FTXUI, and doing without it

The terminal backend uses [FTXUI](https://github.com/ArthurSonzogni/FTXUI),
pinned to `v7.0.3` and fetched by `FetchContent` at configure time. It is
used for three things — a screen buffer, an input decoder and a resize
signal — and deliberately not for a fourth: **its layout engine is not
used**, because lens owns tiling itself (`src/layout/`). That is roughly a
tenth of FTXUI's surface, and the tenth least likely to move under us.

FTXUI appears in exactly one file, `src/term/ftxui-terminal.cpp`, behind
`ITerminal`. A test greps for that (gate G1.5), so replacing it — with
hand-rolled ANSI, or with anything else — is a change to one file and no
caller.

```sh
cmake -S lens -B build/lens -DLENS_BUILD_TERM=OFF
```

builds everything above the terminal seam and runs every gate except the
ones that need a screen. This is not a courtesy: the tiling solver, the
command table, the help generation and all thirteen golden screens are
provable without a terminal, and making them depend on a git fetch would
couple the testable parts of lens to the one part that is not.

## Running

```sh
unify-lens [options]
  --layout NAME         start in a named layout: browse, run, debug, full
  --geometry COLSxROWS  force geometry; required with --script
  --script FILE         replay a key script, dump the screen, exit
  --session             start a Unify engine (default interactively)
  --welcome             open help at startup (default interactively)
  --no-session / --no-welcome
  -h, --help            show this text
```

Below **80×24** lens exits with a message rather than rendering something
illegible.

### `--script`

Not only a test hook. It makes any lens bug reproducible by a file, which
matters more than usual for a program whose bugs are otherwise reported as
"the screen looked wrong":

```sh
$ cat > repro.keys <<'EOF'
C-x 3        # split side by side
C-x 2        # split the new tile above/below
resize 80x24 # watch it degrade
EOF
$ unify-lens --geometry 120x40 --layout browse --script repro.keys
```

One step per line, spelled exactly as the keymap spells it; `#` comments and
blank lines are ignored; `type <text>` types literal text and
`resize COLSxROWS` is a step too. The final screen goes to stdout.

### `--trace`, for interaction bugs

The screen dump records characters, not attributes — so a selection that
moves changes nothing you can see in it. `--trace` prints what a user
perceives after *every* step instead of the final frame:

```sh
$ unify-lens --geometry 80x24 --script repro.keys --trace=model
Down           focus=Help line=15 top=13 row=2 rows=3 of=20 tiles=5
Up             focus=Help line=14 top=13 row=1 rows=3 of=20 tiles=5
```

`line` is the selection, `top` the first line shown, `row` where the
highlight lands. A key that should move the selection and leaves `row` and
`top` both unchanged is a key that did nothing. `--trace=screen` prints the
whole grid per step when you need the film rather than the summary.

## Tests

| Suite | What it covers |
| --- | --- |
| `lens-layout` | The tiling tree and the geometry solver, including a seeded property test that asserts the coverage invariant cell by cell over random layouts at five geometries. |
| `lens-modreg` | The command table, keymaps, and the help surfaces generated from them. |
| `lens-grid` | The character grid: UTF-8, display width, clipping, boxes. |
| `lens-shell` | `fold`, the Help panel and the `M-x` palette: contextual help, no dead links, palette modality and filtering. |
| `lens-interaction` | What happens *between* frames: that a keystroke visibly does something, that scrolling is minimal, that a modal panel gives focus back — plus a seeded random walk asserting those invariants after every key. See the file's comment for why the other two categories cannot see these bugs. |
| `lens-pty-interaction` | The real `unify-lens` binary on a real pseudo-terminal: that a keystroke produces a frame *without* a second keystroke, and that `C-x C-c` leaves cleanly. The only test that exercises `src/term/`; POSIX only, skipped where no pty can be opened. |
| `lens-screen-*` | Seventeen golden screens: four stock layouts at 120×40 and 80×24, plus tiling gestures, maximise, a resize round trip, an unfinished chord, help at both geometries, and the palette open and filtered. |
| `lens-too-small` | That lens refuses below 80×24 — and renders at exactly 80×24, so the gate is not an off-by-one. |
| `lens-resize-roundtrip` | That shrinking to 80×24 and back restores the screen *exactly*, not merely to something valid. |
| `lens-layering` | That FTXUI stays inside `src/term/`, and that `model/` and `panels/` include no engine header. |

Regenerate the golden screens after a deliberate change:

```sh
UNIFY_UPDATE_GOLDEN=1 ctest --test-dir build/lens -R lens-screen
```

Same convention as the engine's golden tests, deliberately: a CI log reads
the same way whichever suite produced it.

**What the goldens are for, and what they are not for.** They check that a
panel renders, that the stock layouts are what they claim to be, and that
80×24 degrades rather than clips. They are *not* the oracle for behaviour —
that lives in the model tests, so a cosmetic change does not force a dozen
re-recordings.

## Structure

```
src/
  app/      composition root: argv, the model, the loop
  model/    Model, Event, the cell grid, view  -- pure
  modreg/   command table, keymaps, generated help  -- pure
  layout/   tiling tree and geometry solver  -- pure
  term/     FTXUI, and nothing else in the tree sees it
```

The dependency rule is one arrow: `app → panels → model`, and `app → term`.
`model/` and `panels/` see the session boundary types and their own types
and nothing else — no engine header, no Boost, no FTXUI. `layout/` and
`modreg/` are held to the stricter rule of no dependencies at all.

This is what makes both the golden-screen harness and the framework swap
possible, and it is checked mechanically by `test/check-layering.sh` rather
than by convention.

## Status

Gate **G0** (the session boundary) is closed; see
[`../plans/todo/lens/ACCEPTANCE.md`](../plans/todo/lens/ACCEPTANCE.md).
Gate **G1** (the shell) is closed: tiling, the four stock layouts,
degradation, `--script`, the command table, the generated help surfaces, the
Help panel `F1` opens and the `M-x` command palette.

Gate **G2** (the transcript) is under way: define a fact, ask a question,
see the binding, against a real engine. Parse errors and program output land
in the transcript rather than on lens's streams.

On a fresh start lens opens the **first-steps** help page in the main working
area — the keys that matter, and a worked example to type. `C-x 0` dismisses
it and hands the tile back to whatever it was showing; `--no-welcome` starts
without it.

**Most panels do not exist yet.** What the other tiles show is placeholder
text naming what each will show and which gate brings it. That is
deliberate: the shell is gated before the panels so that no panel has to
invent a window manager, and so help exists before there is anything to
explain.
