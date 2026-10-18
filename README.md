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
- **M3 (playable at 30 fps):** done. `astralchain.ini` holds the controls, display and audio
  settings, and saves live in `$XDG_DATA_HOME/astralchain/`. The play checklist
  (`docs/checklist.md`) passed with 0 fallbacks.
- **M4 (60 fps and internal resolution):** done. `fps60 = true` runs the game at 60 fps
  with cutscenes and gameplay at the right speed; `resolution_scale` raises the internal
  resolution. The 60 fps rows of the play checklist passed.
- **Next:** M5 (ImGui overlay).

See `docs/superpowers/specs/2026-10-14-astralchainre-design.md` for the design and roadmap.

## Requirements

- Linux x86_64 on an x86-64-v3 CPU (AVX2 and FMA; any Ryzen, Intel since Haswell). The
  lifted guest code is built for that level; configure with `-DACRE_RECOMP_ARCH=x86-64`
  for an older CPU, at a cost in speed.
- clang 22 for the lifted C (`acre.py build` uses it) and GCC 16 or clang 22 for C++
- CMake ≥ 3.22 and Ninja
- SDL3, Vulkan loader, FFmpeg, Boost, OpenSSL (`openssl-devel` is also needed by the
  lifter tests)

Other dependencies are fetched by CPM at configure time. Their cache lives in `.cache/cpm`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVulkanHeaders_FORCE_BUNDLED=ON \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=c++
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

Settings live in `~/.config/astralchain/astralchain.ini` (created on the first `run`).
Saves live in `~/.local/share/astralchain/profile`.

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
- On Linux x86-64, `build` inlines the lifted code's guest memory accesses (FM2), which
  ran a city scene about 20% faster. `build --no-fastmem` leaves them out, and
  `fastmem = false` in the ini, or `ACRE_NO_FASTMEM=1`, turns them off at run time.
- `fps60 = true` in the ini turns on the 60 fps patch (`game/patches/fps60.patches`); it
  takes effect on the next start. Combat does not hold 60 everywhere yet.
- `resolution_scale` sets the internal resolution as a multiple of the docked 1080p: 0.5,
  0.75, 1, 1.5, 2 (2160p), 3 or 4. `scaling_filter` picks how the image fits the window.
- `reactive_flushing` is off by default. Turning it on makes the game wait for the GPU
  before reading memory the GPU wrote, which costs about 10% in combat; try it if a
  picture or effect looks stale.
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

## Legal: bring your own copy

This project ships tools and a runtime, never the game.

- The repository contains no game code, assets, keys or saves. What it holds about the game
  is metadata needed to check and patch your own copy: module hashes (`game/version.json`),
  code entry addresses (`game/roots/`), and patch offsets with the few instruction words they
  replace (`game/patches/`).
- You need a copy of the game and console keys that you dumped yourself from your own
  Switch. This project does not explain how to get either, and issues asking for them are
  closed.
- A built `astralchain-static` contains the game's code, translated from your dump. **Do not
  share builds, exports or anything under `work/`.** Share source changes only.
- *Astral Chain* is a trademark of its owners. This project is not affiliated with or
  endorsed by Nintendo or PlatinumGames.

## License

GPL-2.0-or-later, inherited from suyu and yuzu. See `LICENSE.txt` and `LICENSES/`.
