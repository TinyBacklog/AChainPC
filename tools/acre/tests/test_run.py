import tempfile
import unittest
from pathlib import Path

from acre import config, coverage, run

MAIN = ["a" * 64, "b" * 64, "c" * 64]


class VerdictTest(unittest.TestCase):
    def test_timeout_without_failure_passes(self):
        self.assertTrue(run.verdict({"termination": "timeout", "failure": ""}).ok)

    def test_guest_panic_fails(self):
        v = run.verdict({"termination": "guest_panic", "failure": "guest_panic"})
        self.assertFalse(v.ok)
        self.assertIn("guest_panic", v.reason)

    def test_fallback_required_fails(self):
        v = run.verdict({"termination": "aot_fallback_required",
                         "failure": "aot_fallback_required pc=0x1"})
        self.assertFalse(v.ok)

    def test_early_exit_without_failure_still_fails(self):
        self.assertFalse(run.verdict({"termination": "load_error", "failure": ""}).ok)


class CoverageTest(unittest.TestCase):
    def test_merges_offsets_sorted_unique(self):
        d = Path(tempfile.mkdtemp())
        (d / "main.roots").write_text("# note\n10\n")
        report = {"modules": [{"name": "main", "sha256": MAIN}],
                  "fallback_sites": [{"module": "main", "offset": 0x20},
                                     {"module": "main", "offset": 0x10},
                                     {"module": "main", "offset": 0x20}]}
        added = coverage.merge(report, d, {"main": MAIN})
        self.assertEqual(added, 1)
        self.assertEqual((d / "main.roots").read_text(), "# note\n10\n20\n")

    def test_rejects_report_from_other_version(self):
        report = {"modules": [{"name": "main", "sha256": ["x", "y", "z"]}], "fallback_sites": []}
        with self.assertRaises(SystemExit) as e:
            coverage.merge(report, Path(tempfile.mkdtemp()), {"main": MAIN})
        self.assertEqual(e.exception.code, config.EXIT_WRONG_VERSION)


class RequirementsTest(unittest.TestCase):
    def test_run_without_build_exits_no_build(self):
        with self.assertRaises(SystemExit) as e:
            run.require_binary(Path("/nonexistent/astralchain-static"))
        self.assertEqual(e.exception.code, config.EXIT_NO_BUILD)


if __name__ == "__main__":
    unittest.main()


class RunBuildsFirstTest(unittest.TestCase):
    def test_incremental_build_runs_before_play(self):
        from unittest import mock
        build_dir = Path(tempfile.mkdtemp())
        (build_dir / "CMakeCache.txt").write_text("")
        with mock.patch("subprocess.run") as fake:
            fake.return_value.returncode = 0
            run.ensure_current(build_dir)
        command = fake.call_args.args[0]
        self.assertEqual(command[:3], ["cmake", "--build", str(build_dir)])
        self.assertIn("astralchain-static", command)

    def test_failed_incremental_build_stops(self):
        from unittest import mock
        build_dir = Path(tempfile.mkdtemp())
        (build_dir / "CMakeCache.txt").write_text("")
        with mock.patch("subprocess.run") as fake:
            fake.return_value.returncode = 1
            with self.assertRaises(SystemExit) as e:
                run.ensure_current(build_dir)
        self.assertEqual(e.exception.code, config.EXIT_BUILD_FAILED)


class StrictCoverageTest(unittest.TestCase):
    def test_offsets_for_module_without_hashes_refused(self):
        report = {"modules": [], "fallback_sites": [{"module": "main", "offset": 0x10}]}
        with self.assertRaises(SystemExit) as e:
            coverage.merge(report, Path(tempfile.mkdtemp()), {"main": MAIN})
        self.assertEqual(e.exception.code, config.EXIT_WRONG_VERSION)
