"""acre.py build: compile the runtime, frontend and work/export into astralchain-static."""
import shutil
import subprocess
from pathlib import Path

from . import config

BUILD_DIR = config.WORK / "build-static"
BINARY = BUILD_DIR / "bin" / "astralchain-static"


def cached_compilers(build_dir: Path) -> dict[str, str]:
    """The C and C++ compilers an existing CMake cache was configured with."""
    found = {}
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            name, sep, value = line.partition("=")
            key = name.split(":", 1)[0]
            if sep and key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"):
                found[key] = value
    return found


def run(jobs: int | None = None, work: Path = config.WORK) -> None:
    export = work / "export"
    if not (export / "recomp_registration.c").is_file():
        config.fail(config.EXIT_NO_EXPORT, f"no export in {export}; run `acre.py export` first")
    for tool in ("cmake", "ninja", "clang", "c++"):
        if shutil.which(tool) is None:
            config.fail(config.EXIT_NO_COMPILER, f"{tool} not found in PATH")
    log = work / "logs" / "build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    # The lifted C is clang's: on the same code it ran a CPU-bound scene at 40 fps
    # against GCC's 33. C++ is pinned to the default compiler, or CMake looks for
    # it next to clang and may take a different toolchain's clang++.
    compilers = {"CMAKE_C_COMPILER": shutil.which("clang"), "CMAKE_CXX_COMPILER": shutil.which("c++")}
    configure = ["cmake", "-S", str(config.REPO), "-B", str(BUILD_DIR), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DVulkanHeaders_FORCE_BUNDLED=ON", "-DACRE_TESTS=OFF",
                 f"-DASTRALCHAIN_RECOMP_DIR={export}",
                 *(f"-D{name}={path}" for name, path in compilers.items())]
    cached = cached_compilers(BUILD_DIR)
    if cached and cached != compilers:
        # A new compiler in an old cache makes CMake wipe the cache and configure
        # again without any of the -D options above.
        configure.insert(1, "--fresh")
    build = ["cmake", "--build", str(BUILD_DIR), "--target", "astralchain-static"]
    if jobs:
        build += ["-j", str(jobs)]
    with log.open("w") as out:
        for command in (configure, build):
            if subprocess.run(command, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                config.fail(config.EXIT_BUILD_FAILED, f"build failed; see {log}")
    print(f"built {BINARY}")
