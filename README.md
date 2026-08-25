# Unify

Unify is the rule-engine language of the vault home-automation system: a
Prolog-family logic language with a C-like surface syntax, in files ending
`.ufy`. This module holds the engine — parser, term/unification core, solver,
job engine — plus a small CLI runner and a golden-output test suite.

| Document | What it is |
| --- | --- |
| [`LANGUAGE.md`](LANGUAGE.md) | Tutorial. Start here if you want to *write* `.ufy` programs. |
| [`SPEC.md`](SPEC.md) | Precise, implementation-cited semantics. |
| [`ROADMAP.md`](ROADMAP.md) | What works, what is sketched, what is planned. |
| `README.md` (this file) | How to build, test and run it. |

The engine is embedded in `combine/applications/stuart-app` in production;
this module's only `main()` is `tools/unify-run.cpp`, the CLI runner used by
the tests and for trying programs out by hand.

---

## Requirements

- **CMake** ≥ 3.16
- **A C++17 compiler.** Verified: GCC on Ubuntu (CI), AppleClang 21 on macOS.
- **Boost** — headers plus the `thread`, `system` and `filesystem` libraries.
  Verified against 1.92 (macOS) and Ubuntu's `libboost-all-dev` (CI). The
  parser is Boost.Spirit Qi, so the headers do most of the work.
- **bash** — only for the golden-test harness (`test/run-golden-test.sh`);
  CMake skips the tests with a warning if it is missing.

The core engine needs **nothing else**. cpprest / `combine/modules/rest-server`
are only for the optional REST frontend, which is off by default.

Installing the dependencies:

```bash
# macOS
brew install cmake boost

# Debian / Ubuntu
sudo apt-get install -y build-essential cmake libboost-all-dev
```

---

## Build

From the repository root:

```bash
cmake -S combine/modules/unify -B build/unify -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/unify --parallel
```

This produces:

- `build/unify/libvault-unify-core.a` — the engine (no cpprest dependency)
- `build/unify/unify-run` — the CLI runner

`build/` is gitignored, so parallel build directories (a sanitizer build, a
Debug build) cost nothing.

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `UNIFY_BUILD_XDEBUG` | `ON` on UNIX, `OFF` elsewhere | The xdebug-style TCP debugger backend. Pure Boost.Asio apart from one POSIX `::access()`. Not needed by the engine or the tests. |
| `UNIFY_BUILD_REST` | `OFF` | The cpprest REST frontend. Needs both cpprestsdk and a CMake-buildable `combine/modules/rest-server`; if either is missing, configure warns and skips instead of failing. |
| `UNIFY_SANITIZE` | *(empty)* | Comma-separated `-fsanitize=` values, e.g. `address,undefined`. Applied directory-wide, so the tests run instrumented too. Not supported under MSVC. |

```bash
# sanitizer build in its own directory
cmake -S combine/modules/unify -B build/unify-asan -DUNIFY_SANITIZE=address,undefined
cmake --build build/unify-asan --parallel
```

### The legacy Boost.Jam build

`Jamfile` (and the repo-root `gen1_Jamroot` / `gen1_unify.sh`) are the
historical Linux-only build, driven by `BOB*` environment variables. CMake is
the supported path; the Jamfile is kept because the surrounding vault modules
still use it. Code shared with those modules — notably
`combine/include/vault/vault.hpp` — stays compilable against the pre-1.66
Boost that build uses, via `BOOST_VERSION` guards.

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
git diff combine/modules/unify/test/golden
```

To add a test, drop the program in `test/conformance/`, add a line to
`UNIFY_CONFORMANCE_TESTS` in `test/CMakeLists.txt`, and generate its golden
file as above. See `test/run-golden-test.sh` for the harness's exit-code
conventions (0 match, 1 mismatch, 77 skip, 2 setup error) and the
`UNIFY_EXPECT_EXIT` hook used by the negative test.

CI runs exactly these three commands on Ubuntu — see
`.github/workflows/unify-ci.yml` and its Forgejo twin `.forgejo/workflows/`
`unify-ci.yml`, **which must be kept in sync**.

---

## Run

```bash
./build/unify/unify-run <program.ufy>
```

Program output (the `print` and `emit` builtins) goes to **stdout**; the
engine's clause/goal trace and any diagnostics go to **stderr**, so
`2>/dev/null` gives you just the program's own output:

```console
$ ./build/unify/unify-run combine/modules/unify/mediaplayer.ufy 2>/dev/null
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
