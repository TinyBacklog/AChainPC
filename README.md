# AstralChainRe

Static recompilation of *Astral Chain* (Nintendo Switch, `01007300020FA000`) into a native
Linux x86_64 executable.

The game's AArch64 code is translated ahead of time into C and compiled for the host. The
translated code runs on a vendored runtime derived from suyu: HLE kernel and services,
`video_core` (Vulkan) and `audio_core`. When the static pass misses an address, dynarmic is
used as a JIT fallback.

**No game code or assets are part of this repository.** To build a playable binary you need
your own dump of the game with update 1.0.1 applied, plus your own console keys.

## Status

- **M1 (foundation):** done.
- **M2 (pipeline):** done. `tools/acre.py` goes from your own dump to a native binary that runs
  fully ahead-of-time compiled.
- **Next:** M3 (playable at 30 fps, configurable controls), then M4 (60 fps and internal
  resolution).

See `docs/superpowers/specs/2026-10-14-astralchainre-design.md` for the design and roadmap.

## Requirements

- Linux x86_64
- GCC 16 or clang 22
- CMake ≥ 3.22 and Ninja
- SDL3, Vulkan loader, FFmpeg, Boost, OpenSSL (`openssl-devel` is also needed by the
  lifter tests)

Other dependencies are fetched by CPM at configure time. Their cache lives in `.cache/cpm`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVulkanHeaders_FORCE_BUNDLED=ON
cmake --build build
```

This builds `build/bin/astralchain` in its JIT-only form.

To build the statically recompiled variant from an existing export, add
`-DASTRALCHAIN_RECOMP_DIR=<export dir>` and build the `astralchain-static` target.

## Playing your copy

You need:

- the base game NSP (`[01007300020FA000][v0]`);
- update 1.0.1 (`[01007300020FA800][v65536]`). Other versions are refused;
- your console keys: a directory with `prod.keys`. The default is `~/.local/share/suyu/keys`.

Build the JIT frontend once, as described in [Build](#build). Then:

```sh
tools/acre.py setup --base "BASE.nsp" --update "UPDATE.nsp" [--keys DIR]
tools/acre.py export          # C from the dumped modules; incremental
tools/acre.py build -j16      # astralchain-static (the first build takes a few minutes)
tools/acre.py run             # play
tools/acre.py run --validate  # 180 s headless, strict AOT: PASS/FAIL
```

What each command does:

- `setup` installs the update into an isolated profile (`work/profile`). It dumps the game's
  modules as the emulator loads them, and checks their hashes against `game/version.json`.
  A modded or older dump is refused.
- If a run reports fallback sites, `tools/acre.py roots --from work/logs/validate-*.json`
  adds them to `game/roots/`, and the next `export` covers them.
- Everything derived from your dump stays in `work/`, which git ignores.

Exit codes:

| Code | Meaning |
|---|---|
| 2 | usage |
| 10 | keys missing |
| 11 | game file missing |
| 12 | frontend not built |
| 13 | update install failed |
| 14 | dump failed |
| 15 | wrong game version |
| 19 | run `setup` first |
| 20 | run `export` first |
| 21 | run `build` first |
| 22 | cmake/ninja missing |
| 23 | export failed |
| 24 | build failed |
| 25 | validation failed |

## Tests

```sh
ctest --test-dir build -L lifter   # lifter: generated C per instruction family
ctest --test-dir build -L acre     # pipeline unit tests (Python, stdlib only)
```

Each lifter test generates a C program that exercises one family of AArch64 instructions,
then compiles and runs it.

## Layout

| Path | Contents |
|---|---|
| `runtime/` | Vendored suyu modules (`src/` upstream), with history |
| `frontend/` | SDL3 frontend (`astralchain` executable) |
| `externals/`, `CMakeModules/`, `cpmfile.json` | Build infrastructure inherited from suyu |
| `tools/tests/` | Lifter tests |
| `tools/acre.py`, `tools/acre/` | Pipeline |
| `recompiler/` | `export_module` generator |
| `game/` | Supported version hashes, extra roots |
| `tools/import-runtime.sh` | Script that produced the vendored runtime |
| `work/` | Local exports and builds (ignored by git) |

## License

GPL-2.0-or-later, inherited from suyu and yuzu. See `LICENSE.txt` and `LICENSES/`.
