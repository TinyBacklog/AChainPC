"""Paths, exit codes and the work/game.json record shared by every acre command."""
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORK = REPO / "work"
DUMP_DIR = WORK / "dump-1.0.1"
VERSION_FILE = REPO / "game" / "version.json"
FRONTEND = REPO / "build" / "bin" / "astralchain"
INI_TEMPLATE = REPO / "frontend" / "astralchain.ini"

EXIT_USAGE = 2
EXIT_NO_KEYS = 10
EXIT_NO_GAME_FILE = 11
EXIT_NO_FRONTEND = 12
EXIT_INSTALL_FAILED = 13
EXIT_DUMP_FAILED = 14
EXIT_WRONG_VERSION = 15
EXIT_NO_SETUP = 19
EXIT_NO_EXPORT = 20
EXIT_NO_BUILD = 21
EXIT_NO_COMPILER = 22
EXIT_EXPORT_FAILED = 23
EXIT_BUILD_FAILED = 24
EXIT_VALIDATE_FAILED = 25
EXIT_PROFILE_MIGRATION = 26
EXIT_EXTRACT_FAILED = 27


def fail(code: int, message: str):
    print(f"acre: {message}", file=sys.stderr)
    raise SystemExit(code)


def default_keys_dir() -> Path:
    data = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(data) / "suyu" / "keys"


def _xdg(variable: str, fallback: str) -> Path:
    return Path(os.environ.get(variable) or str(Path.home() / fallback))


def play_profile() -> Path:
    """NAND and saves of the game being played."""
    return _xdg("XDG_DATA_HOME", ".local/share") / "astralchain" / "profile"


def ini_path() -> Path:
    return _xdg("XDG_CONFIG_HOME", ".config") / "astralchain" / "astralchain.ini"


def save(work: Path, record: dict) -> None:
    work.mkdir(parents=True, exist_ok=True)
    tmp = work / "game.json.tmp"
    tmp.write_text(json.dumps(record, indent=2) + "\n")
    tmp.replace(work / "game.json")


def load(work: Path) -> dict:
    path = work / "game.json"
    if not path.is_file():
        fail(EXIT_NO_SETUP, f"{path} not found; run `acre.py setup` first")
    return json.loads(path.read_text())


def launch_args(record: dict) -> tuple[list[str], list[str]]:
    """Frontend options that go before the extras, and the `-g` target, for a game.json record."""
    if record.get("game_dir"):
        return [], ["-g", str(Path(record["game_dir"]) / "exefs" / "main")]
    return [f"--keys-dir={record['keys_dir']}"], ["-g", record["base"]]
