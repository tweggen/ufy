# Baseline — recorded before any lens work

[HANDOFF.md](HANDOFF.md) §0 asks for this: the numbers every later gate is
diffed against. Recorded 2026-09-06 on the implementation machine, at
`39023de` plus the one CMake fix described in §3.

## 1. Machine and toolchain

| | |
| --- | --- |
| OS | Ubuntu (Linux 7.0.0-30-generic), x86_64 |
| Compiler | GCC 15.3.0 |
| CMake | 4.3.4 |
| Boost | 1.90.0 |
| readline | **absent** — `unify-run`'s REPL falls back to plain stdin ("Unify REPL: GNU readline not found, using plain stdin"). Noted because G2.8 (history interop with `unify-run -i`) is exercised against the plain-stdin path here, not the readline one. |

## 2. Results

```
cmake -S unify -B build/unify -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/unify --parallel
ctest --test-dir build/unify --output-on-failure
```

- **`ctest`: 23/23 passed, 0 failed.** Total 0.49 s. The suite is
  `unify-golden-*`: 5 non-conformance goldens (including `parse-error`)
  plus 18 conformance goldens.
- **Warnings: zero from project code** under `-Wall -Wextra`. The engine's
  existing bar is met.
- **Sanitizers (`-DUNIFY_SANITIZE=address,undefined`): clean.** 23/23
  passed, 2.00 s. Zero ASan reports, zero UBSan `runtime error:` lines,
  **zero LeakSanitizer reports** with `detect_leaks=1`, both under `ctest`
  and running `unify-run unify/test-engine.ufy` directly.

### 2.1 The leak number is zero, and that is not the same as "no leak"

[HANDOFF.md](HANDOFF.md) §4 notes that `~RuntimeContext` **deliberately
leaks the Engine** (`vault-unify.hpp:2253`). LSan does not contradict that:
it reports only *unreachable* blocks at exit, and the leaked Engine is
still reachable from a static/automatic root when the process ends. So the
honest baseline is **"0 LSan-reported leaks, with one known reachable leak
LSan is structurally unable to see"**. E9 (world reset) is the item that
has to make that leak stop being per-`load` rather than per-process; do not
read a continuing zero here as evidence E9 landed correctly. Gate E9's leak
claim on a loop of `load`s and RSS, not on LSan.

### 2.2 Ten Boost deprecation pragmas, from Boost, not from us

The build emits ten `BOOST_HEADER_DEPRECATED` notes: `src/` includes the
classic Spirit shims (`boost/spirit/include/phoenix*.hpp`) rather than
`boost/phoenix/*.hpp`. They come from Boost's headers, not from project
code, so the "zero warnings" bar is intact — but they are noise that will
bury a real warning eventually, and the include rename is a one-line-each
fix. Filed as an aside for ROADMAP Phase 5.5 (the C++03-era Boost
migration), which already owns this code.

## 3. One fix was needed to configure at all

`find_package(Boost REQUIRED COMPONENTS thread system filesystem)`
(`unify/CMakeLists.txt`) **cannot succeed on Boost 1.90**: Boost.System has
been header-only since 1.69, and the distribution ships no
`libboost_system` in any form — not shared, not static, and no
`boost_system` CMake config package. Both `find_package` modes fail with
`missing components: system`.

Fixed by asking for it as an `OPTIONAL_COMPONENTS` and linking
`Boost::system` only `if(TARGET Boost::system)`, so old and new Boost both
work. This is a genuine portability bug that predates the lens plan, not an
artifact of this machine's setup; it is the reason the "first, do this"
step in HANDOFF §0 does not literally work as written on a current distro.

## 4. Caveat on this machine's Boost

Boost's *runtime* libraries were installed system-wide but the *development*
headers were not, and root was unavailable. Boost 1.90's dev packages were
therefore unpacked into a session-local prefix and the build pointed at it:

```
-DCMAKE_POLICY_DEFAULT_CMP0167=OLD -DBoost_NO_BOOST_CMAKE=ON \
-DBOOST_ROOT=<prefix>/usr -DBOOST_INCLUDEDIR=<prefix>/usr/include \
-DBOOST_LIBRARYDIR=<prefix>/usr/lib/x86_64-linux-gnu \
-DBoost_USE_STATIC_LIBS=ON
```

Same Boost, same version, statically linked. It is not a different build in
any way that should matter — but if a later result is surprising, install
`libboost-all-dev` properly and re-check before believing it.
