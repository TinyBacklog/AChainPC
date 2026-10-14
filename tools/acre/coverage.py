"""acre.py roots --from: turn a run's fallback sites into extra roots for the next export."""
import json
from pathlib import Path

from . import config


def merge(report: dict, roots_dir: Path, expected: dict[str, list[str]]) -> int:
    """Adds the report's fallback offsets to roots_dir/<module>.roots; returns how many were new."""
    reported = {m["name"]: m["sha256"] for m in report.get("modules", [])}
    for name, hashes in reported.items():
        if name in expected and hashes != expected[name]:
            config.fail(config.EXIT_WRONG_VERSION,
                        f"report module {name} is not the supported version; refusing its offsets")
    added = 0
    by_module: dict[str, set[int]] = {}
    for site in report.get("fallback_sites", []):
        if site["module"] in expected:
            by_module.setdefault(site["module"], set()).add(int(site["offset"]))
    for name, offsets in by_module.items():
        path = roots_dir / f"{name}.roots"
        comments, values = [], set()
        if path.is_file():
            for line in path.read_text().splitlines():
                body = line.split("#", 1)[0].strip()
                if body:
                    values.add(int(body, 16))
                elif line.strip():
                    comments.append(line)
        added += len(offsets - values)
        values |= offsets
        roots_dir.mkdir(parents=True, exist_ok=True)
        path.write_text("".join(c + "\n" for c in comments) +
                        "".join(f"{v:x}\n" for v in sorted(values)))
    return added


def run(report_path: Path) -> None:
    from .export import GAME_ROOTS
    expected = json.loads(config.VERSION_FILE.read_text())["modules"]
    added = merge(json.loads(report_path.read_text()), GAME_ROOTS, expected)
    print(f"{added} new roots added to {GAME_ROOTS}" + ("; run `acre.py export`" if added else ""))
