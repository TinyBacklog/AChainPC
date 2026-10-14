import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from acre import profile

TEMPLATE = "[controls]\n; steam_controller, keyboard or custom.\npreset = keyboard\n"


def fake_usb(root: Path, devices):
    for i, (vendor, product) in enumerate(devices):
        d = root / f"7-{i}"
        d.mkdir(parents=True)
        (d / "idVendor").write_text(vendor + "\n")
        (d / "idProduct").write_text(product + "\n")
    return root


class MigrateTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.old, self.new = self.root / "work" / "profile", self.root / "data" / "astralchain" / "profile"
        save = self.old / "nand" / "user" / "save" / "slot0"
        save.parent.mkdir(parents=True)
        save.write_text("progress")

    def test_moves_saves(self):
        self.assertTrue(profile.migrate(self.old, self.new))
        self.assertEqual((self.new / "nand/user/save/slot0").read_text(), "progress")
        self.assertFalse(self.old.exists())

    def test_existing_destination_is_left_alone(self):
        self.new.mkdir(parents=True)
        self.assertFalse(profile.migrate(self.old, self.new))
        self.assertTrue((self.old / "nand/user/save/slot0").exists())

    def test_failed_copy_keeps_source(self):
        with mock.patch("shutil.copytree", side_effect=OSError("disk full")):
            with self.assertRaises(SystemExit):
                profile.migrate(self.old, self.new)
        self.assertEqual((self.old / "nand/user/save/slot0").read_text(), "progress")
        self.assertFalse(self.new.exists())

    def test_nothing_to_migrate(self):
        self.assertFalse(profile.migrate(self.root / "missing", self.new))


class PresetTest(unittest.TestCase):
    def test_steam_controller_detected(self):
        usb = fake_usb(Path(tempfile.mkdtemp()), [("1d6b", "0002"), ("28de", "1304")])
        self.assertEqual(profile.detect_preset(usb), "steam_controller")

    def test_keyboard_otherwise(self):
        usb = fake_usb(Path(tempfile.mkdtemp()), [("1d6b", "0002")])
        self.assertEqual(profile.detect_preset(usb), "keyboard")
        self.assertEqual(profile.detect_preset(Path("/nonexistent")), "keyboard")


class EnsureIniTest(unittest.TestCase):
    def test_creates_with_preset(self):
        path = Path(tempfile.mkdtemp()) / "cfg" / "astralchain.ini"
        self.assertTrue(profile.ensure_ini(path, TEMPLATE, "steam_controller"))
        text = path.read_text()
        self.assertIn("preset = steam_controller\n", text)
        self.assertIn("; steam_controller, keyboard or custom.", text)

    def test_never_overwrites(self):
        path = Path(tempfile.mkdtemp()) / "astralchain.ini"
        path.write_text("mine")
        self.assertFalse(profile.ensure_ini(path, TEMPLATE, "steam_controller"))
        self.assertEqual(path.read_text(), "mine")


if __name__ == "__main__":
    unittest.main()
