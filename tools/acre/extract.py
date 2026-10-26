"""acre.py extract: the effective 1.0.1 (base + update) as exefs/ + romfs/ folders, for testing."""
import shutil
import time
from pathlib import Path

from . import config, setup


def run(base: Path, update: Path, keys_dir: Path, work: Path = config.WORK,
        frontend: Path = config.FRONTEND) -> Path:
    base, update, keys_dir = base.resolve(), update.resolve(), keys_dir.resolve()
    setup.check_file(base, "base game")
    setup.check_file(update, "update")
    setup.check_keys(keys_dir)
    if not frontend.is_file():
        config.fail(config.EXIT_NO_FRONTEND,
                    f"{frontend} not built; run `cmake --build build --target astralchain`")

    # A throwaway profile holds the update; the play profile is never touched.
    profile = work / "extract-profile"
    dump = work / config.DUMP_DIR.name
    new = dump.with_name(dump.name + ".new")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    common = [f"--user-dir={profile}", f"--keys-dir={keys_dir}"]
    setup.fresh_profile(profile)

    log = work / "logs" / f"extract-install-{stamp}.log"
    if setup._frontend(frontend, log, *common, f"--install={update}") != 0:
        config.fail(config.EXIT_INSTALL_FAILED, f"installing the update failed; see {log}")

    # The frontend refuses an existing destination, so a leftover of an interrupted run goes.
    if new.exists():
        shutil.rmtree(new)
    log = work / "logs" / f"extract-game-{stamp}.log"
    rc = setup._frontend(frontend, log, *common, f"--extract-game={new}", "-g", str(base))
    if rc != 0 or not (new / "exefs" / "main").is_file():
        config.fail(config.EXIT_EXTRACT_FAILED, f"extracting the game failed (rc={rc}); see {log}")

    if dump.exists():
        shutil.rmtree(dump)
    new.rename(dump)
    print(f"game extracted: {dump}")
    return dump
