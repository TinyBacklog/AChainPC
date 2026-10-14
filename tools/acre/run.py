"""acre.py run: play, or --validate a headless strict run against the AOT build."""
import json
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from . import config
from .build import BINARY


@dataclass
class Verdict:
    ok: bool
    reason: str


def verdict(report: dict) -> Verdict:
    termination, failure = report.get("termination", ""), report.get("failure", "")
    if failure:
        return Verdict(False, failure)
    if termination != "timeout":
        return Verdict(False, f"run ended early: {termination or 'unknown'}")
    return Verdict(True, "ran until the timeout without fallbacks")


def require_binary(binary: Path) -> None:
    if not binary.is_file():
        config.fail(config.EXIT_NO_BUILD, f"{binary} not built; run `acre.py build` first")


def run(validate: bool = False, seconds: int = 180, extra: list[str] | None = None,
        work: Path = config.WORK) -> None:
    record = config.load(work)
    require_binary(BINARY)
    command = [str(BINARY), f"--user-dir={record['profile']}", f"--keys-dir={record['keys_dir']}"]
    if not validate:
        command += [*(extra or []), "-g", record["base"]]
        raise SystemExit(subprocess.run(command).returncode)

    stamp = time.strftime("%Y%m%d-%H%M%S")
    logs = work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    report_path, log = logs / f"validate-{stamp}.json", logs / f"validate-{stamp}.log"
    command += ["--headless", f"--headless-timeout={seconds}", "--recomp-strict",
                f"--recomp-stats={report_path}", "-g", record["base"]]
    with log.open("w") as out:
        subprocess.run(command, stdout=out, stderr=subprocess.STDOUT)
    if not report_path.is_file():
        config.fail(config.EXIT_VALIDATE_FAILED, f"no report written; see {log}")
    report = json.loads(report_path.read_text())
    cores = report.get("cores", [])
    blocks = sum(c.get("aot_blocks", 0) for c in cores)
    fallbacks = sum(c.get("fallback_requests", 0) for c in cores)
    result = verdict(report)
    print(f"{'PASS' if result.ok else 'FAIL'}: {result.reason} "
          f"({blocks} AOT blocks, {fallbacks} fallbacks, {seconds}s)\nreport: {report_path}")
    if not result.ok:
        raise SystemExit(config.EXIT_VALIDATE_FAILED)
