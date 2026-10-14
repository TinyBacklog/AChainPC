"""Checks that dumped NSO segments are exactly the supported game version."""
import hashlib
import json
from pathlib import Path

SEGMENTS = ("text", "rodata", "data")


class IdentityError(Exception):
    pass


def load_expected(path: Path) -> dict:
    return json.loads(Path(path).read_text())


def module_hashes(module_dir: Path) -> list[str]:
    return [hashlib.sha256((module_dir / "data" / f"{segment}.bin").read_bytes()).hexdigest()
            for segment in SEGMENTS]


def verify(inputs_dir: Path, expected: dict) -> None:
    """Raises IdentityError naming the first module and segment that differ."""
    version = expected.get("version", "the supported version")
    others = expected.get("known_other_versions", {})
    for name, wanted in expected["modules"].items():
        module_dir = Path(inputs_dir) / name
        if not all((module_dir / "data" / f"{s}.bin").is_file() for s in SEGMENTS):
            raise IdentityError(f"module {name}: segments missing in {module_dir}")
        actual = module_hashes(module_dir)
        if actual == wanted:
            continue
        if name == "main" and actual[0][:12] in others:
            raise IdentityError(
                f"game version {others[actual[0][:12]]} detected; "
                f"update {version} is not applied")
        segment = next(s for s, a, w in zip(SEGMENTS, actual, wanted) if a != w)
        raise IdentityError(
            f"module {name}: {segment} segment does not match {version} "
            f"(patched or modded dump?)")
