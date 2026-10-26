import tempfile
import unittest
from pathlib import Path
from unittest import mock

from acre import config


class ExtractTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.work = self.root / "work"
        self.base = self.root / "base.nsp"
        self.update = self.root / "update.nsp"
        self.keys = self.root / "keys"
        self.frontend = self.root / "astralchain"
        for f in (self.base, self.update, self.frontend):
            f.write_text("x")
        self.keys.mkdir()
        (self.keys / "prod.keys").write_text("x")
        self.calls = []
        self.extract_rc = 0

    def fake_frontend(self, frontend, log, *args):
        """Records the arguments; --extract-game writes a minimal game folder."""
        self.calls.append(list(args))
        for arg in args:
            if arg.startswith("--extract-game="):
                if self.extract_rc != 0:
                    return self.extract_rc
                out = Path(arg.split("=", 1)[1])
                self.assertFalse(out.exists(), "the frontend refuses an existing destination")
                (out / "exefs").mkdir(parents=True)
                (out / "exefs" / "main").write_text("m")
                (out / "exefs" / "main.npdm").write_text("n")
                (out / "romfs").mkdir()
                (out / "romfs" / "x").write_text("r")
        return 0

    def run_extract(self, **overrides):
        from acre import extract
        kwargs = dict(base=self.base, update=self.update, keys_dir=self.keys,
                      work=self.work, frontend=self.frontend)
        kwargs.update(overrides)
        with mock.patch("acre.setup._frontend", side_effect=self.fake_frontend):
            return extract.run(**kwargs)

    def user_dir(self, call):
        return Path(next(a for a in call if a.startswith("--user-dir=")).split("=", 1)[1])

    def test_extract_uses_throwaway_profile(self):
        stale = self.work / "extract-profile" / "load" / "stale"
        stale.mkdir(parents=True)
        self.run_extract()
        for call in self.calls:
            profile = self.user_dir(call)
            self.assertEqual(profile, self.work / "extract-profile")
            self.assertNotEqual(profile, config.play_profile())
        self.assertFalse(stale.exists())

    def test_extract_installs_update_then_extracts(self):
        self.run_extract()
        self.assertEqual(len(self.calls), 2)
        self.assertIn(f"--install={self.update.resolve()}", self.calls[0])
        self.assertIn(f"--extract-game={self.work / 'dump-1.0.1.new'}", self.calls[1])
        g = self.calls[1].index("-g")
        self.assertEqual(self.calls[1][g + 1], str(self.base.resolve()))

    def test_extract_replaces_old_dump(self):
        old = self.work / "dump-1.0.1" / "velho"
        old.mkdir(parents=True)
        dump = self.run_extract()
        self.assertEqual(dump, self.work / "dump-1.0.1")
        self.assertFalse(old.exists())
        self.assertTrue((dump / "exefs" / "main").is_file())
        self.assertTrue((dump / "romfs" / "x").is_file())
        self.assertFalse((self.work / "dump-1.0.1.new").exists())

    def test_extract_clears_leftover_new(self):
        leftover = self.work / "dump-1.0.1.new" / "half"
        leftover.mkdir(parents=True)
        dump = self.run_extract()
        self.assertTrue((dump / "exefs" / "main").is_file())
        self.assertFalse(leftover.exists())

    def test_extract_failure_keeps_old_dump(self):
        old = self.work / "dump-1.0.1" / "velho"
        old.mkdir(parents=True)
        self.extract_rc = 1
        with self.assertRaises(SystemExit) as e:
            self.run_extract()
        self.assertEqual(e.exception.code, 27)
        self.assertEqual(config.EXIT_EXTRACT_FAILED, 27)
        self.assertTrue(old.is_dir())

    def test_missing_keys_exits_10(self):
        with self.assertRaises(SystemExit) as e:
            self.run_extract(keys_dir=self.root / "nokeys")
        self.assertEqual(e.exception.code, 10)
        self.assertEqual(self.calls, [])

    def test_missing_update_exits_11(self):
        with self.assertRaises(SystemExit) as e:
            self.run_extract(update=self.root / "missing.nsp")
        self.assertEqual(e.exception.code, 11)
        self.assertEqual(self.calls, [])

    def test_dump_dir_is_under_work(self):
        self.assertEqual(config.DUMP_DIR, config.WORK / "dump-1.0.1")


if __name__ == "__main__":
    unittest.main()
