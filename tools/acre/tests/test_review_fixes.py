"""Regression tests for the M2 final review findings."""
import json
import os
import stat
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from acre import config, identity, run, setup
from acre.tests.test_identity import h, make_module


def fake_frontend(directory: Path, body: str) -> Path:
    """A stand-in for the astralchain binary: a Python script with the given body."""
    path = directory / "fake-frontend"
    path.write_text(f"#!{sys.executable}\nimport sys, pathlib, time, json\nargs = sys.argv[1:]\n{body}\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


DUMPER = '''
for a in args:
    if a.startswith("--dump-recomp-inputs="):
        d = pathlib.Path(a.split("=", 1)[1]) / "main"
        (d / "data").mkdir(parents=True)
        (d / "module.json").write_text("{}")
        for s in ("text", "rodata", "data"):
            (d / "data" / (s + ".bin")).write_bytes(b"x")
'''


class SetupKeepsSavesTest(unittest.TestCase):
    def test_rerunning_setup_keeps_play_profile_saves(self):
        root = Path(tempfile.mkdtemp())
        work, keys = root / "work", root / "keys"
        keys.mkdir()
        (keys / "prod.keys").write_text("k")
        base = root / "base.nsp"
        base.write_text("b")
        save = work / "profile" / "nand" / "user" / "save" / "slot0"
        save.parent.mkdir(parents=True)
        save.write_text("progress")
        frontend = fake_frontend(root, DUMPER)
        with mock.patch.object(identity, "verify"), \
             mock.patch.object(identity, "load_expected",
                               return_value={"version": "1.0.1", "modules": {"main": []}}):
            setup.run(base, None, keys, work=work, frontend=frontend)
        moved = config.play_profile() / "nand" / "user" / "save" / "slot0"
        self.assertEqual(moved.read_text(), "progress")
        self.assertTrue(str(config.play_profile()).startswith(os.environ["XDG_DATA_HOME"]))


class ExtraModuleTest(unittest.TestCase):
    def test_unexpected_module_is_refused(self):
        tmp = Path(tempfile.mkdtemp())
        make_module(tmp, "main")
        make_module(tmp, "subsdk0")
        expected = {"modules": {"main": [h(b"t"), h(b"r"), h(b"d")]}}
        with self.assertRaisesRegex(identity.IdentityError, r"subsdk0.*not part"):
            identity.verify(tmp, expected)


class ValidateHardeningTest(unittest.TestCase):
    def test_non_positive_seconds_refused(self):
        for value in (0, -5):
            with self.assertRaises(SystemExit) as e:
                run.check_seconds(value)
            self.assertEqual(e.exception.code, config.EXIT_USAGE)

    def test_nonzero_exit_fails_even_with_timeout_report(self):
        v = run.verdict({"termination": "timeout", "failure": ""}, returncode=139)
        self.assertFalse(v.ok)
        self.assertIn("139", v.reason)

    def test_hung_frontend_is_killed(self):
        root = Path(tempfile.mkdtemp())
        binary = fake_frontend(root, "time.sleep(60)")
        start = time.monotonic()
        with self.assertRaises(SystemExit) as e:
            run.validate([str(binary)], root / "report.json", root / "log.txt",
                         seconds=1, margin=1)
        self.assertEqual(e.exception.code, config.EXIT_VALIDATE_FAILED)
        self.assertLess(time.monotonic() - start, 20)


if __name__ == "__main__":
    unittest.main()
