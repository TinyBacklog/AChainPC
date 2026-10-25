"""Reads an ACRE_FRAME_TRACE file (runtime/common/frame_trace.h) and explains
the frames that stayed on screen for two vsync periods.

python3 -m acre.frametrace TRACE [--last SECONDS]
"""
from __future__ import annotations

import argparse
import bisect
from dataclasses import dataclass, field
from pathlib import Path

MS = 1_000_000
# The game waited this long for a free buffer before it counts as held back by
# the display side rather than by its own work.
DEQUEUE_STALL_NS = 2 * MS
# The vsync event reached the game this late after the timer fired.
LATE_SIGNAL_NS = 2 * MS


@dataclass
class Interval:
    """One emulated vsync period, from one timer tick to the next."""
    tick: int
    end: int
    gpu_idle: int = 0
    core_idle: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    compose_begin: int | None = None
    compose_end: int | None = None
    vsync: int | None = None
    acquired: bool = False
    queues: list[int] = field(default_factory=list)


@dataclass
class Report:
    intervals: int
    repeats: int
    causes: dict[str, int]
    game_frames_ms: list[float]
    signal_delay_ms: list[float]
    compose_ms: list[float]
    gpu_busy_normal: list[float]
    gpu_busy_before_repeat: list[float]
    core_busy_normal: list[list[float]]
    core_busy_before_repeat: list[list[float]]
    lateness_ms: list[float]
    dequeue_wait_ms: list[float]
    render_ms: list[float]
    render_frame_wait_ms: list[float]
    present_ms: list[float]


def parse(text: str) -> list[tuple[int, str, int]]:
    events = []
    for line in text.splitlines():
        parts = line.split(",")
        if len(parts) != 3 or not parts[0].isdigit():
            continue
        events.append((int(parts[0]), parts[1], int(parts[2])))
    events.sort(key=lambda e: e[0])
    return events


def _pairs(events, begin: str, end: str) -> list[tuple[int, int]]:
    out, start = [], None
    for ns, name, _ in events:
        if name == begin:
            start = ns
        elif name == end and start is not None:
            out.append((start, ns))
            start = None
    return out


def _intervals(events) -> list[Interval]:
    ticks = [(ns, value) for ns, name, value in events if name == "tick"]
    intervals = [Interval(tick=a[0], end=b[0], gpu_idle=b[1]) for a, b in zip(ticks, ticks[1:])]
    starts = [i.tick for i in intervals]
    by_end = {i.end: i for i in intervals}
    for ns, name, value in events:
        if name.startswith("idle"):
            # Written with the tick that closes the interval it covers.
            if ns in by_end:
                by_end[ns].core_idle[int(name[4:])] = value
            continue
        index = bisect.bisect_right(starts, ns) - 1
        if index < 0 or ns >= intervals[index].end:
            continue
        interval = intervals[index]
        if name == "compose0" and interval.compose_begin is None:
            interval.compose_begin = ns
        elif name == "compose1":
            interval.compose_end = ns
        elif name == "vsync":
            interval.vsync = ns
        elif name == "acquire":
            interval.acquired = True
        elif name == "queue":
            interval.queues.append(ns)
    return intervals


def analyze(events, last_seconds: float | None = None) -> Report:
    if last_seconds is not None and events:
        cutoff = events[-1][0] - int(last_seconds * 1e9)
        events = [e for e in events if e[0] >= cutoff]
    intervals = [i for i in _intervals(events) if i.compose_begin is not None]
    queues = [ns for ns, name, _ in events if name == "queue"]
    dequeues = _pairs(events, "dequeue0", "dequeue1")

    causes: dict[str, int] = {}
    lateness, busy_normal, busy_repeat = [], [], []
    cores_normal: list[list[float]] = [[], [], [], []]
    cores_repeat: list[list[float]] = [[], [], [], []]
    repeats = 0
    for previous, current in zip(intervals, intervals[1:]):
        length = max(previous.end - previous.tick, 1)
        busy = 1 - previous.gpu_idle / length
        cores = [1 - idle / length for idle in previous.core_idle]
        if current.acquired:
            busy_normal.append(busy)
            for core, value in enumerate(cores):
                cores_normal[core].append(value)
            continue
        repeats += 1
        busy_repeat.append(busy)
        for core, value in enumerate(cores):
            cores_repeat[core].append(value)
        # Was a frame queued after the last composition and before this one?
        delivered = [q for q in queues if previous.compose_begin <= q < current.compose_begin]
        if delivered:
            cause = "queued in time, not shown"
        else:
            later = [q for q in queues if q >= current.compose_begin]
            if later:
                lateness.append((later[0] - current.compose_begin) / MS)
            waited = sum(min(e, current.compose_begin) - max(s, previous.compose_begin)
                         for s, e in dequeues if e > previous.compose_begin and s < current.compose_begin)
            if waited >= DEQUEUE_STALL_NS:
                cause = "game late: waited for a free buffer"
            elif previous.vsync is not None and previous.vsync - previous.tick >= LATE_SIGNAL_NS:
                cause = "game late: vsync signalled late"
            else:
                cause = "game late: its own work"
        causes[cause] = causes.get(cause, 0) + 1

    renders = _pairs(events, "render0", "render1")
    frames = _pairs(events, "render0", "rframe")
    presents = _pairs(events, "present0", "present1")
    return Report(
        intervals=len(intervals),
        repeats=repeats,
        causes=causes,
        game_frames_ms=[(b - a) / MS for a, b in zip(queues, queues[1:])],
        signal_delay_ms=[(i.vsync - i.tick) / MS for i in intervals if i.vsync is not None],
        compose_ms=[(i.compose_end - i.compose_begin) / MS for i in intervals if i.compose_end is not None],
        gpu_busy_normal=busy_normal,
        gpu_busy_before_repeat=busy_repeat,
        core_busy_normal=cores_normal,
        core_busy_before_repeat=cores_repeat,
        lateness_ms=lateness,
        dequeue_wait_ms=[(e - s) / MS for s, e in dequeues],
        render_ms=[(e - s) / MS for s, e in renders],
        render_frame_wait_ms=[(e - s) / MS for s, e in frames],
        present_ms=[(e - s) / MS for s, e in presents],
    )


def _quantiles(values: list[float]) -> str:
    if not values:
        return "no samples"
    ordered = sorted(values)

    def at(q: float) -> float:
        return ordered[min(len(ordered) - 1, int(q * len(ordered)))]

    return f"p50 {at(0.5):.2f}  p90 {at(0.9):.2f}  p99 {at(0.99):.2f}  max {ordered[-1]:.2f}  (n={len(ordered)})"


def format_report(report: Report) -> list[str]:
    lines = [f"vsync periods: {report.intervals}, repeated (frame shown twice): {report.repeats}"]
    for cause, count in sorted(report.causes.items(), key=lambda c: -c[1]):
        lines.append(f"  {count:6d}  {cause}")
    buckets = {"<=17.5": 0, "17.5-20": 0, "20-34": 0, ">34": 0}
    for ms in report.game_frames_ms:
        key = "<=17.5" if ms <= 17.5 else "17.5-20" if ms <= 20 else "20-34" if ms <= 34 else ">34"
        buckets[key] += 1
    lines.append("game frame (queue to queue) ms: " + "  ".join(f"{k}: {v}" for k, v in buckets.items()))
    lines.append("game frame ms:            " + _quantiles(report.game_frames_ms))
    lines.append("late frame, ms after compose: " + _quantiles(report.lateness_ms))
    lines.append("tick to vsync signal ms:  " + _quantiles(report.signal_delay_ms))
    lines.append("compose ms:               " + _quantiles(report.compose_ms))
    lines.append("dequeue wait ms:          " + _quantiles(report.dequeue_wait_ms))
    lines.append("GPU thread busy, shown:   " + _quantiles([b * 100 for b in report.gpu_busy_normal]) + " %")
    lines.append("GPU thread busy, before repeat: " + _quantiles([b * 100 for b in report.gpu_busy_before_repeat]) + " %")
    for core in range(4):
        lines.append(f"core {core} busy, shown:    " + _quantiles([b * 100 for b in report.core_busy_normal[core]]) + " %")
        lines.append(f"core {core} busy, before repeat: "
                     + _quantiles([b * 100 for b in report.core_busy_before_repeat[core]]) + " %")
    lines.append("render ms:                " + _quantiles(report.render_ms))
    lines.append("render, wait free frame ms: " + _quantiles(report.render_frame_wait_ms))
    lines.append("present ms:               " + _quantiles(report.present_ms))
    return lines


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--last", type=float, default=None, help="only the last SECONDS of the trace")
    args = parser.parse_args(argv)
    events = parse(args.trace.read_text(errors="replace"))
    print("\n".join(format_report(analyze(events, args.last))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
