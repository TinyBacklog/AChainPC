"""acre.py run: play, or --validate a headless strict run against the AOT build."""
import json
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from . import config
from . import profile as profiles
from .build import BINARY


@dataclass
class Verdict:
    ok: bool
    reason: str


def verdict(report: dict, returncode: int = 0) -> Verdict:
    termination, failure = report.get("termination", ""), report.get("failure", "")
    if failure:
        return Verdict(False, failure)
    if returncode != 0:
        return Verdict(False, f"frontend exited with {returncode} after a {termination or 'unknown'} report")
    if termination != "timeout":
        return Verdict(False, f"run ended early: {termination or 'unknown'}")
    return Verdict(True, "ran until the timeout without fallbacks")


def require_binary(binary: Path) -> None:
    if not binary.is_file():
        config.fail(config.EXIT_NO_BUILD, f"{binary} not built; run `acre.py build` first")


def check_seconds(seconds: int) -> None:
    if seconds <= 0:
        config.fail(config.EXIT_USAGE, "--seconds must be positive")


def validate(command: list[str], report_path: Path, log: Path, seconds: int, margin: int = 120) -> None:
    """Runs a headless strict session; fails on a bad report, a bad exit or a hang."""
    command = [*command, "--headless", f"--headless-timeout={seconds}", "--recomp-strict",
               f"--recomp-stats={report_path}"]
    with log.open("w") as out:
        try:
            returncode = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT,
                                        timeout=seconds + margin).returncode
        except subprocess.TimeoutExpired:
            config.fail(config.EXIT_VALIDATE_FAILED,
                        f"frontend still running {margin}s past --seconds; killed. See {log}")
    if not report_path.is_file():
        config.fail(config.EXIT_VALIDATE_FAILED, f"no report written; see {log}")
    report = json.loads(report_path.read_text())
    cores = report.get("cores", [])
    blocks = sum(c.get("aot_blocks", 0) for c in cores)
    fallbacks = sum(c.get("fallback_requests", 0) for c in cores)
    result = verdict(report, returncode)
    print(f"{'PASS' if result.ok else 'FAIL'}: {result.reason} "
          f"({blocks} AOT blocks, {fallbacks} fallbacks, {seconds}s)\nreport: {report_path}")
    if not result.ok:
        raise SystemExit(config.EXIT_VALIDATE_FAILED)


def run(validate_run: bool = False, seconds: int = 180, extra: list[str] | None = None,
        work: Path = config.WORK) -> None:
    record = config.load(work)
    require_binary(BINARY)
    play_profile, ini = profiles.prepare(record, work)
    command = [str(BINARY), f"--user-dir={play_profile}", f"--keys-dir={record['keys_dir']}",
               f"--ini={ini}"]
    if not validate_run:
        command += [*(extra or []), "-g", record["base"]]
        raise SystemExit(subprocess.run(command).returncode)

    check_seconds(seconds)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    logs = work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    validate([*command, *(extra or []), "-g", record["base"]],
             logs / f"validate-{stamp}.json", logs / f"validate-{stamp}.log", seconds)
