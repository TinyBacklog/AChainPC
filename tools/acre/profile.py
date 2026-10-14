"""Where the player's data lives: the profile (NAND, saves) and astralchain.ini."""
import shutil
from pathlib import Path

from . import config

STEAM_CONTROLLER_PUCK = ("28de", "1304")


def migrate(old: Path, new: Path) -> bool:
    """Moves a profile to its new home. The source goes only after a full copy exists."""
    if new.exists() or not old.is_dir():
        return False
    tmp = new.with_name(new.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copytree(old, tmp, symlinks=True)
    except OSError as error:
        shutil.rmtree(tmp, ignore_errors=True)
        config.fail(config.EXIT_PROFILE_MIGRATION,
                    f"could not copy {old} to {new} ({error}); nothing was removed")
    tmp.rename(new)
    shutil.rmtree(old)
    return True


def detect_preset(usb_root: Path = Path("/sys/bus/usb/devices")) -> str:
    try:
        devices = list(usb_root.iterdir())
    except OSError:
        return "keyboard"
    for device in devices:
        try:
            ids = ((device / "idVendor").read_text().strip(), (device / "idProduct").read_text().strip())
        except OSError:
            continue
        if ids == STEAM_CONTROLLER_PUCK:
            return "steam_controller"
    return "keyboard"


def ensure_ini(path: Path, template: str, preset: str) -> bool:
    """Creates astralchain.ini from the template with the given preset; never overwrites."""
    if path.exists():
        return False
    lines = [f"preset = {preset}" if line.strip().startswith("preset =") else line
             for line in template.splitlines()]
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text("\n".join(lines) + "\n")
    tmp.replace(path)
    return True


def prepare(record: dict, work: Path = config.WORK) -> tuple[Path, Path]:
    """Play profile and ini for a run: migrates the legacy work/profile, creates the ini.

    Only work/profile is ever moved. game.json's "profile" is just updated, never
    trusted as something to move and delete: it could name any directory."""
    target = config.play_profile()
    legacy = work / "profile"
    if migrate(legacy, target):
        print(f"profile moved: {legacy} -> {target}")
    elif legacy.is_dir():
        print(f"note: {legacy} left in place; {target} already exists")
    if record.get("profile") != str(target):
        record["profile"] = str(target)
        config.save(work, record)
    ini = config.ini_path()
    preset = detect_preset()
    if ensure_ini(ini, config.INI_TEMPLATE.read_text(), preset):
        print(f"created {ini} (controls: {preset})")
    return target, ini
