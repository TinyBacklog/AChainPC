import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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


PARTS = ["exefs/main", "exefs/main.npdm", "exefs/rtld", "exefs/sdk", "romfs"]


def make_game_dir(root: Path, skip: str | None = None) -> Path:
    game = root / "game"
    for part in PARTS:
        if part == skip:
            continue
        path = game / part
        if part == "romfs":
            path.mkdir(parents=True)
            (path / "x").write_text("r")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("x")
    return game


class GameDirTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.work = self.root / "work"
        self.frontend = self.root / "astralchain"
        self.frontend.write_text("x")
        self.game = make_game_dir(self.root)
        self.calls = []
        env = mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(self.root / "data"),
                                           "XDG_CONFIG_HOME": str(self.root / "cfg")})
        env.start()
        self.addCleanup(env.stop)

    def fake_frontend(self, frontend, log, *args):
        self.calls.append(list(args))
        for arg in args:
            if arg.startswith("--dump-recomp-inputs="):
                out = Path(arg.split("=", 1)[1])
                (out / "main").mkdir(parents=True)
                (out / "main" / "module.json").write_text("{}")
        return 0

    def run_game_dir(self):
        with mock.patch("acre.setup._frontend", side_effect=self.fake_frontend), \
                mock.patch("acre.identity.verify"), \
                mock.patch("acre.identity.load_expected",
                           return_value={"version": "1.0.1", "modules": {"main": []}}):
            setup.run_game_dir(self.game, work=self.work, frontend=self.frontend)

    def test_game_dir_missing_parts(self):
        for part in PARTS:
            with self.subTest(part=part):
                game = make_game_dir(Path(tempfile.mkdtemp()), skip=part)
                with self.assertRaises(SystemExit) as e:
                    setup.check_game_dir(game)
                self.assertEqual(e.exception.code, config.EXIT_NO_GAME_FILE)

    def test_game_dir_missing_part_is_named(self):
        for part in PARTS:
            with self.subTest(part=part):
                game = make_game_dir(Path(tempfile.mkdtemp()), skip=part)
                with mock.patch("sys.stderr") as err, self.assertRaises(SystemExit):
                    setup.check_game_dir(game)
                written = "".join(c.args[0] for c in err.write.call_args_list)
                self.assertIn(part, written)

    def test_game_dir_empty_romfs_counts_as_missing(self):
        (self.game / "romfs" / "x").unlink()
        with self.assertRaises(SystemExit) as e:
            setup.check_game_dir(self.game)
        self.assertEqual(e.exception.code, config.EXIT_NO_GAME_FILE)

    def test_complete_game_dir_passes(self):
        setup.check_game_dir(self.game)

    def test_game_dir_dump_has_no_keys_and_no_install(self):
        self.run_game_dir()
        self.assertEqual(len(self.calls), 1)
        call = self.calls[0]
        self.assertTrue(any(a.startswith("--dump-recomp-inputs=") for a in call))
        self.assertEqual(call[-2:], ["-g", str(self.game.resolve() / "exefs" / "main")])
        self.assertFalse(any(a.startswith(("--keys-dir", "--install")) for a in call))
        self.assertIn(f"--user-dir={self.work / 'setup-profile'}", call)

    def test_game_dir_record(self):
        self.run_game_dir()
        record = config.load(self.work)
        self.assertEqual(record["game_dir"], str(self.game.resolve()))
        self.assertTrue(Path(record["game_dir"]).is_absolute())
        self.assertIsNone(record["keys_dir"])
        self.assertEqual(record["inputs"], str(self.work / "inputs"))
        self.assertIn("profile", record)
        self.assertTrue((self.work / "inputs" / "main" / "module.json").is_file())

    def test_launch_args_game_dir(self):
        before, target = config.launch_args({"game_dir": "/g", "keys_dir": None})
        self.assertEqual(before, [])
        self.assertEqual(target, ["-g", str(Path("/g") / "exefs" / "main")])

    def test_launch_args_legacy_record(self):
        before, target = config.launch_args({"base": "/b.nsp", "keys_dir": "/k"})
        self.assertEqual(before, ["--keys-dir=/k"])
        self.assertEqual(target, ["-g", "/b.nsp"])


if __name__ == "__main__":
    unittest.main()
