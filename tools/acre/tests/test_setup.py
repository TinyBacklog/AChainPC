import tempfile
import unittest
from pathlib import Path

from acre import config, setup


class SetupTest(unittest.TestCase):
    def test_missing_keys_dir_exits_10(self):
        with self.assertRaises(SystemExit) as e:
            setup.check_keys(Path("/nonexistent"))
        self.assertEqual(e.exception.code, 10)

    def test_keys_dir_needs_prod_keys(self):
        d = Path(tempfile.mkdtemp())
        with self.assertRaises(SystemExit) as e:
            setup.check_keys(d)
        self.assertEqual(e.exception.code, 10)
        (d / "prod.keys").write_text("x")
        setup.check_keys(d)

    def test_missing_game_file_exits_11(self):
        with self.assertRaises(SystemExit) as e:
            setup.check_file(Path("/nonexistent/game.nsp"), "base")
        self.assertEqual(e.exception.code, 11)

    def test_paths_with_brackets_survive_config_roundtrip(self):
        w = Path(tempfile.mkdtemp())
        cfg = {"base": "/a/ASTRAL CHAIN [01007300020FA000][v0].nsp"}
        config.save(w, cfg)
        self.assertEqual(config.load(w)["base"], cfg["base"])

    def test_load_without_setup_exits_with_hint(self):
        with self.assertRaises(SystemExit) as e:
            config.load(Path(tempfile.mkdtemp()))
        self.assertEqual(e.exception.code, config.EXIT_NO_SETUP)

    def test_fresh_profile_drops_stale_mods(self):
        w = Path(tempfile.mkdtemp())
        stale = w / "profile" / "load" / "01007300020FA000" / "60fps"
        stale.mkdir(parents=True)
        setup.fresh_profile(w / "profile")
        self.assertTrue((w / "profile").is_dir())
        self.assertFalse(stale.exists())


if __name__ == "__main__":
    unittest.main()
