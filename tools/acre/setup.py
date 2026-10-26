"""acre.py setup: install the update, dump the effective modules, check they are 1.0.1."""
import shutil
import subprocess
import time
from pathlib import Path

from . import config, identity
from . import profile as profiles


def check_keys(keys_dir: Path) -> None:
    if not (keys_dir / "prod.keys").is_file():
        config.fail(config.EXIT_NO_KEYS,
                    f"no prod.keys in {keys_dir}; pass --keys DIR with your console keys")


def check_file(path: Path, what: str) -> None:
    if not path.is_file():
        config.fail(config.EXIT_NO_GAME_FILE, f"{what} file not found: {path}")


def fresh_profile(profile: Path) -> None:
    """A new profile every time: a stale mod in load/ would end up in the dump."""
    if profile.exists():
        shutil.rmtree(profile)
    profile.mkdir(parents=True)


def check_game_dir(game_dir: Path) -> None:
    """The folder `extract` writes: exefs/{main,main.npdm,rtld,sdk} plus a non-empty romfs/."""
    for part in ("main", "main.npdm", "rtld", "sdk"):
        if not (game_dir / "exefs" / part).is_file():
            config.fail(config.EXIT_NO_GAME_FILE, f"exefs/{part} not found in {game_dir}")
    romfs = game_dir / "romfs"
    if not romfs.is_dir() or not any(romfs.iterdir()):
        config.fail(config.EXIT_NO_GAME_FILE, f"romfs/ missing or empty in {game_dir}")


def _frontend(frontend: Path, log: Path, *args: str) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as out:
        return subprocess.run([str(frontend), *args], stdout=out, stderr=subprocess.STDOUT).returncode


def _need_frontend(frontend: Path) -> None:
    if not frontend.is_file():
        config.fail(config.EXIT_NO_FRONTEND,
                    f"{frontend} not built; run `cmake --build build --target astralchain`")


def _dump_and_verify(frontend: Path, work: Path, stamp: str, args: list[str], target: str) -> dict:
    """Dump the game modules to work/inputs, check they are the supported version, return the expected identity."""
    inputs, tmp = work / "inputs", work / "inputs.tmp"
    if tmp.exists():
        shutil.rmtree(tmp)
    log = work / "logs" / f"setup-dump-{stamp}.log"
    rc = _frontend(frontend, log, *args, f"--dump-recomp-inputs={tmp}", "-g", target)
    if rc != 0 or not any(tmp.glob("*/module.json")):
        config.fail(config.EXIT_DUMP_FAILED, f"dumping the game modules failed (rc={rc}); see {log}")

    expected = identity.load_expected(config.VERSION_FILE)
    try:
        identity.verify(tmp, expected)
    except identity.IdentityError as error:
        config.fail(config.EXIT_WRONG_VERSION, str(error))

    if inputs.exists():
        shutil.rmtree(inputs)
    tmp.rename(inputs)
    return expected


def run(base: Path, update: Path | None, keys_dir: Path, work: Path = config.WORK,
        frontend: Path = config.FRONTEND) -> None:
    base, keys_dir = base.resolve(), keys_dir.resolve()
    check_file(base, "base game")
    if update is not None:
        update = update.resolve()
        check_file(update, "update")
    check_keys(keys_dir)
    _need_frontend(frontend)

    # The dump runs in a throwaway profile, so no stale mod can reach it. The play
    # profile holds the player's saves and is never wiped; it only gets the update.
    profile, dump_profile = config.play_profile(), work / "setup-profile"
    if profiles.migrate(work / "profile", profile):
        print(f"profile moved: {work / 'profile'} -> {profile}")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    keys = f"--keys-dir={keys_dir}"
    common = [f"--user-dir={dump_profile}", keys]
    fresh_profile(dump_profile)
    profile.mkdir(parents=True, exist_ok=True)

    if update is not None:
        for target in (dump_profile, profile):
            log = work / "logs" / f"setup-install-{target.name}-{stamp}.log"
            if _frontend(frontend, log, f"--user-dir={target}", keys, f"--install={update}") != 0:
                config.fail(config.EXIT_INSTALL_FAILED, f"installing the update failed; see {log}")
        print(f"update installed: {update.name}")

    expected = _dump_and_verify(frontend, work, stamp, common, str(base))
    config.save(work, {"base": str(base), "update": str(update) if update else None,
                       "keys_dir": str(keys_dir), "profile": str(profile),
                       "inputs": str(work / "inputs")})
    print(f"inputs verified: {expected['version']} ({', '.join(expected['modules'])})")


def run_game_dir(game_dir: Path, work: Path = config.WORK, frontend: Path = config.FRONTEND) -> None:
    game_dir = game_dir.resolve()
    check_game_dir(game_dir)
    _need_frontend(frontend)

    # No keys and no update: the folder already is the effective 1.0.1. The dump still runs in a
    # throwaway profile so no mod from load/ reaches it.
    profile, dump_profile = config.play_profile(), work / "setup-profile"
    if profiles.migrate(work / "profile", profile):
        print(f"profile moved: {work / 'profile'} -> {profile}")
    fresh_profile(dump_profile)
    profile.mkdir(parents=True, exist_ok=True)

    stamp = time.strftime("%Y%m%d-%H%M%S")
    expected = _dump_and_verify(frontend, work, stamp, [f"--user-dir={dump_profile}"],
                                str(game_dir / "exefs" / "main"))
    config.save(work, {"game_dir": str(game_dir), "keys_dir": None, "profile": str(profile),
                       "inputs": str(work / "inputs")})
    print(f"inputs verified: {expected['version']} ({', '.join(expected['modules'])})")
