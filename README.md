# ufy

The **Unify** logic language (`.ufy`) and its engine, factored out of the
[vault](https://github.com/tweggen/vault) home-automation repository, where it
lived as `combine/modules/unify`. The full commit history of that directory is
preserved here.

| Path | What it is |
| --- | --- |
| [`unify/`](unify/) | The engine: parser, term/unification core, solver, job engine, CLI runner, golden-output test suite. **Start at [`unify/README.md`](unify/README.md).** |
| `include/vault/` | Shared headers vendored from the vault repository's `combine/include/`. Only `vault.hpp` is here — `vault-unify-xdebug-tcp.cpp` needs `vault::BoostAsioIoService` from it. Keep it in sync with vault by hand. |
| `.github/`, `.forgejo/` | CI: build + golden tests + an ASan/UBSan job, on Ubuntu. The two workflow files are twins and **must be kept in sync**. |

## Quick start

```bash
cmake -S unify -B build/unify -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/unify --parallel
ctest --test-dir build/unify --output-on-failure
./build/unify/unify-run unify/mediaplayer.ufy 2>/dev/null
```

Requires CMake ≥ 3.16, a C++17 compiler and Boost (headers plus `thread`,
`system`, `filesystem`). See [`unify/README.md`](unify/README.md) for build
options, the test workflow and platform notes,
[`unify/LANGUAGE.md`](unify/LANGUAGE.md) for the tutorial,
[`unify/SPEC.md`](unify/SPEC.md) for the semantics and
[`unify/ROADMAP.md`](unify/ROADMAP.md) for what is planned.
