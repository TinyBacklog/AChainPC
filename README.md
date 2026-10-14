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

Milestone M1 (foundation) is done: the repository builds on its own, and the lifter tests
pass. The `dump → export → build` pipeline (`tools/acre.py`) is milestone M2. See
`docs/superpowers/specs/2026-10-14-astralchainre-design.md` for the full design and roadmap.

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

## Tests

```sh
ctest --test-dir build -L lifter
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
| `tools/import-runtime.sh` | Script that produced the vendored runtime |
| `work/` | Local exports and builds (ignored by git) |

## License

GPL-2.0-or-later, inherited from suyu and yuzu. See `LICENSE.txt` and `LICENSES/`.
