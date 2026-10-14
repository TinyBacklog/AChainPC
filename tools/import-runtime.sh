#!/bin/sh
# Rebuilds the vendored runtime from a suyu checkout, keeping its history.
# Usage: tools/import-runtime.sh <suyu-repo> <branch> <out-dir>
set -eu
src=$1; branch=$2; out=$3
rm -rf "$out"
git clone --no-local --single-branch --branch "$branch" "$src" "$out"
cd "$out"
git filter-repo --force --invert-paths \
  --path src/android --path src/suyu --path src/yuzu --path src/yuzu_cmd \
  --path src/qt_common --path src/libretro_core --path src/dedicated_room \
  --path src/yuzu_room_standalone --path src/web_service --path src/tests \
  --path .github --path .gitea --path .forgejo --path .ci --path .gitlab-ci.yml \
  --path .agents --path .claude --path .codex --path .cleanthat --path .coderabbit.yaml \
  --path docs --path img --path hooks --path dist/languages --path main --path master \
  --path part --path artifacts --path tools-local --path conanfile.txt \
  --path tools/static_recompiler --path .tmp_diffs --path dist/qt_themes \
  --path vcpkg.json --path vcpkg-configuration.json --path vcpkg-overlays \
  --path externals/vcpkg --path externals/breakpad --path externals/discord-rpc \
  --path externals/cpp-jwt --path externals/libusb --path externals/libadrenotools \
  --path externals/metal-cpp --path externals/nxdumptool \
  --path externals/nx-optimizer --path externals/ownfoil --path externals/nxapi \
  --path externals/sse2neon --path externals/dynarmic --path externals/oaknut
git filter-repo --force \
  --path-rename src/suyu_cmd/:frontend/ \
  --path-rename tools/aot/:tools/tests/ \
  --path-rename src/:runtime/
