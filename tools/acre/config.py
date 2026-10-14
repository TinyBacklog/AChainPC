"""Paths, exit codes and the work/game.json record shared by every acre command."""
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORK = REPO / "work"
VERSION_FILE = REPO / "game" / "version.json"
FRONTEND = REPO / "build" / "bin" / "astralchain"

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


def fail(code: int, message: str):
    print(f"acre: {message}", file=sys.stderr)
    raise SystemExit(code)


def default_keys_dir() -> Path:
    data = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(data) / "suyu" / "keys"


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
