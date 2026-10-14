"""acre.py build: compile the runtime, frontend and work/export into astralchain-static."""
import shutil
import subprocess
from pathlib import Path

from . import config

BUILD_DIR = config.WORK / "build-static"
BINARY = BUILD_DIR / "bin" / "astralchain-static"


def run(jobs: int | None = None, work: Path = config.WORK) -> None:
    export = work / "export"
    if not (export / "recomp_registration.c").is_file():
        config.fail(config.EXIT_NO_EXPORT, f"no export in {export}; run `acre.py export` first")
    for tool in ("cmake", "ninja"):
        if shutil.which(tool) is None:
            config.fail(config.EXIT_NO_COMPILER, f"{tool} not found in PATH")
    log = work / "logs" / "build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    configure = ["cmake", "-S", str(config.REPO), "-B", str(BUILD_DIR), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DVulkanHeaders_FORCE_BUNDLED=ON", "-DACRE_TESTS=OFF",
                 f"-DASTRALCHAIN_RECOMP_DIR={export}"]
    build = ["cmake", "--build", str(BUILD_DIR), "--target", "astralchain-static"]
    if jobs:
        build += ["-j", str(jobs)]
    with log.open("w") as out:
        for command in (configure, build):
            if subprocess.run(command, stdout=out, stderr=subprocess.STDOUT).returncode != 0:
                config.fail(config.EXIT_BUILD_FAILED, f"build failed; see {log}")
    print(f"built {BINARY}")
