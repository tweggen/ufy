# Unify

Unify is the rule-engine language of the vault home-automation system: a
Prolog-family logic language with a C-like surface syntax, in files ending
`.ufy`. This module holds the engine — parser, term/unification core, solver,
job engine — plus a small CLI runner with an interactive REPL, and a
golden-output test suite.

| Document | What it is |
| --- | --- |
| [`LANGUAGE.md`](LANGUAGE.md) | Tutorial. Start here if you want to *write* `.ufy` programs. |
| [`SPEC.md`](SPEC.md) | Precise, implementation-cited semantics. |
| [`ROADMAP.md`](ROADMAP.md) | What works, what is sketched, what is planned. |
| `README.md` (this file) | How to build, test and run it. |

The engine is embedded in an application in production (`combine/applications/`
`stuart-app` in the vault repository, which this directory was factored out of);
this module's only `main()` is `tools/unify-run.cpp`, the CLI runner used by
the tests and — through its REPL (`tools/unify-repl.cpp`) — for trying
programs out by hand.

---

## Requirements

- **CMake** ≥ 3.16
- **A C++17 compiler.** Verified: GCC on Ubuntu (CI), AppleClang 21 on macOS.
- **Boost** — headers plus the `thread`, `system` and `filesystem` libraries.
  Verified against 1.92 (macOS) and Ubuntu's `libboost-all-dev` (CI). The
  parser is Boost.Spirit Qi, so the headers do most of the work.
- **bash** — only for the golden-test harness (`test/run-golden-test.sh`);
  CMake skips the tests with a warning if it is missing.
- **GNU readline** — *optional*, and only for the REPL, which gains line
  editing, history and `~/.unify_history` when it is found at configure
  time. Without it the REPL reads plain lines from stdin and everything
  else is unchanged.

The core engine needs **nothing else**.

Installing the dependencies:

```bash
# macOS
brew install cmake boost
brew install readline            # optional, for the REPL

# Debian / Ubuntu
sudo apt-get install -y build-essential cmake libboost-all-dev
sudo apt-get install -y libreadline-dev                # optional, for the REPL
```

---

## Build

From the repository root:

```bash
cmake -S unify -B build/unify -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/unify --parallel
```

This produces:

- `build/unify/libvault-unify-core.a` — the engine
- `build/unify/unify-run` — the CLI runner

`build/` is gitignored, so parallel build directories (a sanitizer build, a
Debug build) cost nothing.

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `UNIFY_BUILD_XDEBUG` | `ON` on UNIX, `OFF` elsewhere | The xdebug-style TCP debugger backend. Pure Boost.Asio apart from one POSIX `::access()`. Not needed by the engine or the tests. |
| `UNIFY_SANITIZE` | *(empty)* | Comma-separated `-fsanitize=` values, e.g. `address,undefined`. Applied directory-wide, so the tests run instrumented too. Not supported under MSVC. |
| `UNIFY_USE_READLINE` | `ON` | Link GNU readline into `unify-run`, *if it can be found* (Homebrew's keg-only install included), giving the REPL line editing and history. Configure prints which way it went. `OFF` forces the plain-stdin fallback. |

```bash
# sanitizer build in its own directory
cmake -S unify -B build/unify-asan -DUNIFY_SANITIZE=address,undefined
cmake --build build/unify-asan --parallel
```

### The legacy Boost.Jam build

`Jamfile` is the historical Linux-only build, driven by `BOB*` environment
variables and by a `Jamroot` that lives in the vault repository, not here. It
is kept for reference only; CMake is the supported path. Code shared with the
vault modules — notably `../include/vault/vault.hpp`, vendored from that
repository's `combine/include/` — stays compilable against the pre-1.66 Boost
that build uses, via `BOOST_VERSION` guards.

---

## Test

```bash
ctest --test-dir build/unify --output-on-failure
```

23 tests: 5 sample programs, 1 negative parse-error test, and 17 conformance
programs (`test/conformance/*.ufy`, one per rule documented in `SPEC.md`).

Each test runs a `.ufy` program through `unify-run` and diffs its **stdout**
against `test/golden/<name>.expected`. Trailing whitespace and trailing blank
lines are normalized away; everything else must match exactly. A test whose
golden file does not exist yet reports **SKIP**, not failure.

After a deliberate language or output change, regenerate the goldens and
review every diff before committing — they are the pass/fail baseline:

```bash
UNIFY_UPDATE_GOLDEN=1 ctest --test-dir build/unify
git diff unify/test/golden
```

To add a test, drop the program in `test/conformance/`, add a line to
`UNIFY_CONFORMANCE_TESTS` in `test/CMakeLists.txt`, and generate its golden
file as above. See `test/run-golden-test.sh` for the harness's exit-code
conventions (0 match, 1 mismatch, 77 skip, 2 setup error) and the
`UNIFY_EXPECT_EXIT` hook used by the negative test.

CI runs exactly these three commands on Ubuntu — see the repository root's
`.github/workflows/unify-ci.yml` and its Forgejo twin `.forgejo/workflows/`
`unify-ci.yml`, **which must be kept in sync**.

---

## Run

```bash
./build/unify/unify-run <program.ufy>          # run a program and exit
./build/unify/unify-run                        # interactive REPL
./build/unify/unify-run -i <program.ufy>       # run it, then stay in the REPL
```

### Batch mode

Program output (the `print` and `emit` builtins) goes to **stdout**; the
engine's clause/goal trace and any diagnostics go to **stderr**, so
`2>/dev/null` gives you just the program's own output:

```console
$ ./build/unify/unify-run unify/mediaplayer.ufy 2>/dev/null
print: unify mediaplayer: booting multi-room controller
print: default zone: kitchen
print: zones:
print:   - Kitchen (kitchen)
...
print: unify mediaplayer: done
```

Exit codes: `0` success, `1` parse or unification errors were reported
(details on stderr), `2` usage error or the file could not be opened.

Programs worth running:

- `mediaplayer.ufy` — the flagship demo: a multi-room media-player controller
  exercising rules, cut, arithmetic, `findall`, loops, `assert`/`retract` and
  string handling.
- `test/conformance/*.ufy` — one small program per language feature, each with
  its expected output derived by hand in its own header comment.
- `pathfinder.ufy` — calls home-automation driver builtins that do not exist
  in `vault-unify-core`, so it parses but does not fully solve.

### The REPL

Given no program (or `-i`), `unify-run` starts an interactive session
(`tools/unify-repl.cpp`). The prompt takes anything a `.ufy` file takes —
facts, rules, `query { ... }` blocks, `import` — against one engine that
keeps everything for the whole session, so definitions accumulate and later
queries see them. Multi-line input continues on a `...>` prompt until the
item is closed; an empty line abandons a half-typed one.

Two things exist only at the prompt: `? <goals>` as shorthand for
`query { <goals> }`, and a finished query reporting its **variable
bindings**, one line per solution, followed by a count — so a query is
useful without having to litter it with `print(...)`.

```console
$ ./build/unify/unify-run
Unify REPL. :help for help, :quit to leave.
ufy> color( red );
ufy> color( green );
ufy> warm( $x ) {
...>     color( $x );
...> }
ufy> ? warm( $w );
$w = red
$w = green
-- 2 solutions
ufy> :list warm
warm( $x (VT30) ) :- color( $x (VT30) ).
ufy> :quit
```

Commands, all `:`-prefixed: `:help`, `:list [name]` (the clauses defined so
far, builtins and desugaring artefacts filtered out), `:load <file>` (like
`import`, but it re-reads a file already loaded), `:quit`. Ctrl-D also
leaves.

An interactive session turns the engine's stderr trace **off** — a prompt
buried under a page of trace is not a prompt, and silencing it with
`2>/dev/null` instead would throw away parse-error diagnostics too.
`--trace` puts it back. Batch mode is untouched either way.

The REPL also works on a pipe (`unify-run < session.ufy`), where it prints
no banner and no prompts, and exits `1` if anything in the session failed to
parse or hit a unification error.

---

## Platform notes

macOS is supported and tested by hand, but **CI is Linux-only** — a Boost
upgrade that breaks the macOS build will not be caught automatically. Two
known rough edges, neither macOS-specific:

- The build is not warning-clean under clang (`-Wmismatched-tags`,
  `-Wdeprecated-copy-with-user-provided-copy`, and libc++ on Spirit's
  `char_traits<const char>`). These come from C++03-era code that ROADMAP
  Phase 5.5 targets; it *is* warning-clean under GCC.
- Building with `UNIFY_BUILD_XDEBUG=OFF` sidesteps the oldest Boost.Asio code
  in the module if a future Boost breaks it again.
