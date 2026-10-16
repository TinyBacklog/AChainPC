"""acre.py bench: repeatable A/B of builds, driven by a recorded TAS script.

Each run starts from a fresh copy of a profile snapshot (so saves never drift
between runs and the player's own profile is never touched), plays the script
on player 1, waits for the frontend's "tas: script finished" line, lets the
scene settle, then measures the game's frame rate from the log and, when perf
is available, CPU counters for a fixed window. Builds alternate run by run
(A B A B ...), so a slow drift of the machine hits both alike.
"""
import json
import re
import shutil
import signal
import statistics
import subprocess
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from . import config

BENCH_DIR = config.WORK / "bench"
# The frontend logs a frame-rate line every 2 s; a log that stops growing for
# this long means the game stopped presenting.
HANG_SECONDS = 45
PERF_EVENTS = ["task-clock", "cycles", "instructions", "stalled-cycles-frontend", "L1-icache-load-misses"]

_STAMP = re.compile(r"\[\s*(\d+\.\d+)\]")
_FPS = re.compile(r"FPS: game ([0-9.]+)")
_FINISHED = "tas: script finished"


def fps_samples(log_text: str) -> list[tuple[float, float]]:
    """(log time in seconds, game fps) for every FPS line of a --show-fps log."""
    samples = []
    for line in log_text.splitlines():
        fps, stamp = _FPS.search(line), _STAMP.search(line)
        if fps and stamp:
            samples.append((float(stamp.group(1)), float(fps.group(1))))
    return samples


def finish_time(log_text: str) -> float | None:
    """Log time of the end of the script, or None while it still plays."""
    for line in log_text.splitlines():
        if _FINISHED in line:
            stamp = _STAMP.search(line)
            return float(stamp.group(1)) if stamp else None
    return None


@dataclass
class FpsWindow:
    mean: float
    low: float
    high: float
    samples: int


def fps_window(samples: list[tuple[float, float]], start: float, end: float) -> FpsWindow | None:
    """Frame rate over the samples whose log time falls in [start, end)."""
    values = [fps for t, fps in samples if start <= t < end]
    if not values:
        return None
    return FpsWindow(statistics.fmean(values), min(values), max(values), len(values))


def parse_perf(text: str) -> dict[str, float]:
    """Counter values from `perf stat -x,` output (CSV: value,unit,event,...)."""
    counters = {}
    for line in text.splitlines():
        parts = line.split(",")
        if len(parts) >= 3 and parts[2] in PERF_EVENTS:
            try:
                counters[parts[2]] = float(parts[0])
            except ValueError:
                pass  # "<not supported>" or "<not counted>"
    return counters


@dataclass
class RunResult:
    binary: str
    index: int
    fps: FpsWindow | None
    perf: dict[str, float] = field(default_factory=dict)
    log: str = ""
    hung: bool = False


class Hung(Exception):
    """The game stopped presenting frames."""


def _dump_stacks(pid: int, path: Path) -> None:
    """Every thread's stack, for a run that stopped presenting."""
    if shutil.which("eu-stack"):
        with path.open("w") as out:
            subprocess.run(["eu-stack", "-p", str(pid)], stdout=out, stderr=subprocess.STDOUT, timeout=300)


def _wait_for_finish(log: Path, process: subprocess.Popen, timeout: float) -> float:
    deadline = time.monotonic() + timeout
    size, grown = -1, time.monotonic()
    while time.monotonic() < deadline:
        if process.poll() is not None:
            config.fail(config.EXIT_VALIDATE_FAILED, f"frontend exited before the script ended; see {log}")
        text = log.read_text(errors="replace")
        found = finish_time(text)
        if found is not None:
            return found
        if len(text) != size:
            size, grown = len(text), time.monotonic()
        elif time.monotonic() - grown > HANG_SECONDS:
            raise Hung()
        time.sleep(1)
    raise Hung()


def run_one(binary: Path, index: int, snapshot: Path, script: Path, ini: Path, record: dict,
            settle: int, seconds: int, out_dir: Path) -> RunResult:
    name = f"{index:02d}-{binary.parent.parent.name}"
    profile = out_dir / name / "profile"
    shutil.copytree(snapshot, profile, symlinks=True)
    (profile / "tas").mkdir(exist_ok=True)
    shutil.copyfile(script, profile / "tas" / "script0-1.txt")
    log = out_dir / name / "frontend.log"
    command = [str(binary), f"--user-dir={profile}", f"--keys-dir={record['keys_dir']}", f"--ini={ini}",
               "--show-fps", "--tas-play", "-g", record["base"]]
    with log.open("w") as out:
        process = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT)
    try:
        try:
            finished = _wait_for_finish(log, process, timeout=900)
        except Hung:
            _dump_stacks(process.pid, out_dir / name / "hang-stacks.txt")
            return RunResult(str(binary), index, None, {}, str(log), hung=True)
        time.sleep(settle)
        perf_text = ""
        if shutil.which("perf"):
            perf = subprocess.run(["perf", "stat", "-x,", "-e", ",".join(PERF_EVENTS), "-p", str(process.pid),
                                   "--", "sleep", str(seconds)], capture_output=True, text=True)
            perf_text = perf.stderr
        else:
            time.sleep(seconds)
        samples = fps_samples(log.read_text(errors="replace"))
        if not fps_window(samples, finished + settle + seconds - HANG_SECONDS, finished + settle + seconds):
            _dump_stacks(process.pid, out_dir / name / "hang-stacks.txt")
            return RunResult(str(binary), index, None, {}, str(log), hung=True)
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    samples = fps_samples(log.read_text(errors="replace"))
    window = fps_window(samples, finished + settle, finished + settle + seconds)
    return RunResult(str(binary), index, window, parse_perf(perf_text), str(log))


def summarize(results: list[RunResult]) -> list[str]:
    lines = []
    for binary in dict.fromkeys(r.binary for r in results):
        runs = [r for r in results if r.binary == binary and r.fps]
        hung = sum(1 for r in results if r.binary == binary and r.hung)
        if not runs:
            lines.append(f"{binary}: no measurement" + (f", {hung} hung" if hung else ""))
            continue
        means = [r.fps.mean for r in runs]
        line = (f"{binary}: fps mean {statistics.fmean(means):.1f} over {len(runs)} runs "
                f"(runs {', '.join(f'{m:.1f}' for m in means)}; low {min(r.fps.low for r in runs):.1f})")
        perf = [r.perf for r in runs if "cycles" in r.perf and "instructions" in r.perf]
        if perf:
            cycles = statistics.fmean(p["cycles"] for p in perf)
            instructions = statistics.fmean(p["instructions"] for p in perf)
            line += f"; cycles {cycles / 1e9:.1f}G, instructions {instructions / 1e9:.1f}G, IPC {instructions / cycles:.2f}"
        if hung:
            line += f"; {hung} run(s) HUNG"
        lines.append(line)
    return lines


def run(binaries: list[Path], script: Path, runs: int, snapshot: Path, ini: Path, settle: int,
        seconds: int) -> None:
    for binary in binaries:
        if not binary.is_file():
            config.fail(config.EXIT_NO_BUILD, f"{binary} not found")
    for path, what in ((script, "--script"), (snapshot, "--profile"), (ini, "--ini")):
        if not path.exists():
            config.fail(config.EXIT_USAGE, f"{what}: {path} not found")
    record = config.load(config.WORK)
    out_dir = BENCH_DIR / "runs" / time.strftime("%Y%m%d-%H%M%S")
    out_dir.mkdir(parents=True)
    results = []
    for index in range(runs):
        for binary in binaries:
            result = run_one(binary.resolve(), index, snapshot, script, ini, record, settle, seconds, out_dir)
            fps = "HUNG" if result.hung else f"{result.fps.mean:.1f} fps" if result.fps else "no fps"
            print(f"run {index + 1}/{runs} {binary}: {fps}", flush=True)
            results.append(result)
    (out_dir / "results.json").write_text(json.dumps([asdict(r) for r in results], indent=2))
    for line in summarize(results):
        print(line)
    print(f"results: {out_dir / 'results.json'}")
    print(f"profiles copied per run under {out_dir} (about 210 MB each); delete them when done")
