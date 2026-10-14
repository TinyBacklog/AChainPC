"""acre.py setup: install the update, dump the effective modules, check they are 1.0.1."""
import shutil
import subprocess
import time
from pathlib import Path

from . import config, identity


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


def _frontend(frontend: Path, log: Path, *args: str) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as out:
        return subprocess.run([str(frontend), *args], stdout=out, stderr=subprocess.STDOUT).returncode


def run(base: Path, update: Path | None, keys_dir: Path, work: Path = config.WORK,
        frontend: Path = config.FRONTEND) -> None:
    base, keys_dir = base.resolve(), keys_dir.resolve()
    check_file(base, "base game")
    if update is not None:
        update = update.resolve()
        check_file(update, "update")
    check_keys(keys_dir)
    if not frontend.is_file():
        config.fail(config.EXIT_NO_FRONTEND,
                    f"{frontend} not built; run `cmake --build build --target astralchain`")

    profile, inputs, tmp = work / "profile", work / "inputs", work / "inputs.tmp"
    stamp = time.strftime("%Y%m%d-%H%M%S")
    common = [f"--user-dir={profile}", f"--keys-dir={keys_dir}"]
    fresh_profile(profile)

    if update is not None:
        log = work / "logs" / f"setup-install-{stamp}.log"
        if _frontend(frontend, log, *common, f"--install={update}") != 0:
            config.fail(config.EXIT_INSTALL_FAILED, f"installing the update failed; see {log}")
        print(f"update installed: {update.name}")

    if tmp.exists():
        shutil.rmtree(tmp)
    log = work / "logs" / f"setup-dump-{stamp}.log"
    rc = _frontend(frontend, log, *common, f"--dump-recomp-inputs={tmp}", "-g", str(base))
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
    config.save(work, {"base": str(base), "update": str(update) if update else None,
                       "keys_dir": str(keys_dir), "profile": str(profile), "inputs": str(inputs)})
    print(f"inputs verified: {expected['version']} ({', '.join(expected['modules'])})")
