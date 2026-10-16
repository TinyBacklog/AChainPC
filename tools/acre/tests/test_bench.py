import unittest

from acre import bench

LOG = """\
\x1b[0;37m[   2.007899] Frontend <Info> emu_window_sdl2.cpp:750:WaitEvent: FPS: game 11.8 | system 23.6 | speed 100%\x1b[0m
\x1b[0;37m[ 190.400000] Frontend <Info> acre_tas.cpp:80:AcreTasFrame: tas: script finished\x1b[0m
\x1b[0;37m[ 199.000000] Frontend <Info> emu_window_sdl2.cpp:750:WaitEvent: FPS: game 40.0 | system 40.0 | speed 100%\x1b[0m
\x1b[0;37m[ 201.000000] Frontend <Info> emu_window_sdl2.cpp:750:WaitEvent: FPS: game 44.0 | system 44.0 | speed 100%\x1b[0m
\x1b[0;37m[ 203.000000] Frontend <Info> emu_window_sdl2.cpp:750:WaitEvent: FPS: game 48.0 | system 48.0 | speed 100%\x1b[0m
\x1b[0;37m[ 241.000000] Frontend <Info> emu_window_sdl2.cpp:750:WaitEvent: FPS: game 20.0 | system 20.0 | speed 100%\x1b[0m
"""

PERF = """\
25868.91,msec,task-clock,25868910000,100.00,2.587,CPUs utilized
119723828873,,cycles,25868910000,83.38,4.628,GHz
262805976142,,instructions,25868910000,83.23,2.20,insn per cycle
<not supported>,,stalled-cycles-frontend,0,100.00,,
986266335,,L1-icache-load-misses,25868910000,83.47,,
"""


class BenchTest(unittest.TestCase):
    def test_fps_samples_read_time_and_game_fps(self):
        samples = bench.fps_samples(LOG)
        self.assertEqual(samples[0], (2.007899, 11.8))
        self.assertEqual(len(samples), 5)

    def test_finish_time(self):
        self.assertEqual(bench.finish_time(LOG), 190.4)
        self.assertIsNone(bench.finish_time(LOG.replace("script finished", "playing")))

    def test_window_keeps_only_samples_inside(self):
        window = bench.fps_window(bench.fps_samples(LOG), 200.4, 240.4)
        self.assertEqual(window.samples, 2)
        self.assertAlmostEqual(window.mean, 46.0)
        self.assertEqual((window.low, window.high), (44.0, 48.0))
        self.assertIsNone(bench.fps_window(bench.fps_samples(LOG), 300, 340))

    def test_parse_perf_skips_unsupported(self):
        counters = bench.parse_perf(PERF)
        self.assertEqual(counters["cycles"], 119723828873)
        self.assertEqual(counters["instructions"], 262805976142)
        self.assertNotIn("stalled-cycles-frontend", counters)

    def test_summary_reports_mean_and_ipc(self):
        window = bench.FpsWindow(46.0, 44.0, 48.0, 2)
        results = [bench.RunResult("a", 0, window, {"cycles": 100e9, "instructions": 250e9}),
                   bench.RunResult("a", 1, bench.FpsWindow(42.0, 40.0, 44.0, 2), {}),
                   bench.RunResult("b", 0, None)]
        lines = bench.summarize(results)
        self.assertIn("fps mean 44.0 over 2 runs", lines[0])
        self.assertIn("IPC 2.50", lines[0])
        self.assertEqual(lines[1], "b: no measurement")

    def test_summary_counts_hung_runs(self):
        results = [bench.RunResult("a", 0, bench.FpsWindow(46.0, 44.0, 48.0, 2)),
                   bench.RunResult("a", 1, None, hung=True),
                   bench.RunResult("b", 0, None, hung=True)]
        lines = bench.summarize(results)
        self.assertIn("1 run(s) HUNG", lines[0])
        self.assertEqual(lines[1], "b: no measurement, 1 hung")


if __name__ == "__main__":
    unittest.main()
