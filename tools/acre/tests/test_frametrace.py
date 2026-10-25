import unittest

from acre import frametrace

MS = 1_000_000
PERIOD = 16_666_667


def trace(frames: list[tuple[float, float]]) -> str:
    """Ticks every vsync period; each (queue_ms, dequeue_wait_ms) is one game
    frame queued at that time after waiting that long for a buffer."""
    lines = []
    for n in range(12):
        tick = n * PERIOD
        lines += [f"{tick},tick,{PERIOD // 2}", f"{tick + 100_000},compose0,0", f"{tick + 300_000},compose1,0",
                  f"{tick + 400_000},vsync,0"]
    for queue_ms, wait_ms in frames:
        queue = int(queue_ms * MS)
        lines += [f"{queue - int(wait_ms * MS) - MS},dequeue0,0", f"{queue - MS},dequeue1,0", f"{queue},queue,1"]
    # A frame is acquired at the first composition after it was queued.
    composes = [n * PERIOD + 100_000 for n in range(12)]
    for queue_ms, _ in frames:
        after = [c for c in composes if c > queue_ms * MS]
        if after:
            lines.append(f"{after[0] + 50_000},acquire,0")
    return "\n".join(lines) + "\n"


def in_time(late: float | None = None, wait: float = 0) -> list[tuple[float, float]]:
    """One frame 8 ms into every vsync period; with `late`, the fourth comes
    at that time instead."""
    frames = [(n * PERIOD / MS + 8, 0) for n in range(11)]
    if late is not None:
        frames[3] = (late, wait)
    return frames


class FrameTraceTest(unittest.TestCase):
    def test_every_frame_in_time_has_no_repeats(self):
        report = frametrace.analyze(frametrace.parse(trace(in_time())))
        self.assertEqual(report.repeats, 0)

    def test_frame_late_from_its_own_work(self):
        # The fourth frame misses the composition at 66.8 ms.
        report = frametrace.analyze(frametrace.parse(trace(in_time(late=69))))
        self.assertEqual(report.repeats, 1)
        self.assertEqual(report.causes.get("game late: its own work"), 1)

    def test_frame_late_from_waiting_for_a_buffer(self):
        report = frametrace.analyze(frametrace.parse(trace(in_time(late=69, wait=12))))
        self.assertEqual(report.causes.get("game late: waited for a free buffer"), 1)

    def test_report_formats(self):
        report = frametrace.analyze(frametrace.parse(trace(in_time())))
        self.assertTrue(frametrace.format_report(report)[0].startswith("vsync periods:"))


if __name__ == "__main__":
    unittest.main()
