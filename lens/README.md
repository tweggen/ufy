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

vcpkg's Boost is modular, so if a header turns up missing, install the
matching `boost-<lib>` port rather than reaching for anything larger. The
sledgehammer, `vcpkg install boost`, works and takes a long time.

Two things specific to this codebase on Windows:

- **`UNIFY_BUILD_XDEBUG` already defaults to `OFF`** off UNIX. The xdebug TCP
  backend has a POSIX `::access()` call, so it is excluded rather than
  patched. Nothing else in the engine needs it.
- **Boost.Spirit Qi and MSVC object limits.** The parser is a Qi grammar,
  and Qi grammars are notorious for exhausting MSVC's object-file limits. If
  you get `C1128`, add `/bigobj`:

  ```sh
  cmake -S lens -B build/lens \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DCMAKE_CXX_FLAGS="/bigobj"
  ```

  This is a predicted failure, not an observed one.

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
blank lines are ignored; `resize COLSxROWS` is a step too. The final screen
goes to stdout.

## Tests

| Suite | What it covers |
| --- | --- |
| `lens-layout` | The tiling tree and the geometry solver, including a seeded property test that asserts the coverage invariant cell by cell over random layouts at five geometries. |
| `lens-modreg` | The command table, keymaps, and the help surfaces generated from them. |
| `lens-grid` | The character grid: UTF-8, display width, clipping, boxes. |
| `lens-shell` | `fold`, the Help panel and the `M-x` palette: contextual help, no dead links, palette modality and filtering. |
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

**No panel exists yet.** What the layouts show is placeholder text naming
what each panel will show and which gate brings it. That is deliberate: the
shell is gated before the panels so that no panel has to invent a window
manager, and so help exists before there is anything to explain.
