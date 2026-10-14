import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from acre import config, export


def fixture():
    root = Path(tempfile.mkdtemp())
    inputs = root / "inputs"
    for name in ("rtld", "main"):
        d = inputs / name / "data"
        d.mkdir(parents=True)
        for seg in ("text", "rodata", "data"):
            (d / f"{seg}.bin").write_bytes(f"{name}-{seg}".encode())
    roots_dir = root / "roots"
    roots_dir.mkdir()
    lifter = root / "arm64_to_c.h"
    lifter.write_text("// lifter v1\n")
    return SimpleNamespace(inputs=inputs, roots_dir=roots_dir, lifter_files=[lifter])


class ExportTest(unittest.TestCase):
    def test_hash_stable(self):
        w = fixture()
        self.assertEqual(export.input_hash(w.inputs, w.roots_dir, w.lifter_files),
                         export.input_hash(w.inputs, w.roots_dir, w.lifter_files))

    def test_hash_changes_with_extra_roots(self):
        w = fixture()
        before = export.input_hash(w.inputs, w.roots_dir, w.lifter_files)
        (w.roots_dir / "main.roots").write_text("40\n")
        self.assertNotEqual(before, export.input_hash(w.inputs, w.roots_dir, w.lifter_files))

    def test_hash_changes_with_lifter(self):
        w = fixture()
        before = export.input_hash(w.inputs, w.roots_dir, w.lifter_files)
        w.lifter_files[0].write_text("// lifter v2\n")
        self.assertNotEqual(before, export.input_hash(w.inputs, w.roots_dir, w.lifter_files))

    def test_hash_changes_with_segment(self):
        w = fixture()
        before = export.input_hash(w.inputs, w.roots_dir, w.lifter_files)
        (w.inputs / "main" / "data" / "text.bin").write_bytes(b"patched")
        self.assertNotEqual(before, export.input_hash(w.inputs, w.roots_dir, w.lifter_files))

    def test_registration_lists_modules_in_load_order(self):
        src = export.registration(["rtld", "main", "sdk"])
        self.assertLess(src.index("recomp_image_lookup_rtld"), src.index("recomp_image_lookup_main"))
        self.assertIn("suyu_recomp_static_modules", src)

    def test_extra_roots_parsed_as_hex_ignoring_comments(self):
        w = fixture()
        (w.roots_dir / "main.roots").write_text("# from run 1\n40\n0x80\n\n")
        self.assertEqual(export.extra_roots(w.roots_dir, "main"), {0x40, 0x80})
        self.assertEqual(export.extra_roots(w.roots_dir, "sdk"), set())

    def test_missing_inputs_exit_no_setup(self):
        with self.assertRaises(SystemExit) as e:
            export.require_inputs(Path(tempfile.mkdtemp()))
        self.assertEqual(e.exception.code, config.EXIT_NO_SETUP)


if __name__ == "__main__":
    unittest.main()


class PatchFilesTest(unittest.TestCase):
    def test_split_by_module_and_strip_comments(self):
        d = Path(tempfile.mkdtemp())
        (d / "fps60.patches").write_text(
            "# 60fps\n"
            "main 0x9E8D84 word 0x1A9F0501 0x52800021 ; mov w1,#1\n"
            "main 0x91DC10 hook 0x34000495 acre_fps60_step\n"
            "sdk 0x26EA0 word 0x1A88C128 0x52800028\n")
        split = export.read_patches(d)
        self.assertEqual(split["main"], ["9e8d84 word 1a9f0501 52800021", "91dc10 hook 34000495 acre_fps60_step"])
        self.assertEqual(split["sdk"], ["26ea0 word 1a88c128 52800028"])

    def test_bad_line_exits(self):
        d = Path(tempfile.mkdtemp())
        (d / "x.patches").write_text("main 0x10 word 0x1\n")
        with self.assertRaises(SystemExit) as e:
            export.read_patches(d)
        self.assertEqual(e.exception.code, config.EXIT_EXPORT_FAILED)

    def test_branch_replacement_targets_become_roots(self):
        # cbz w8, +0x30 at 0x57E7E0 -> 0x57E810, and the fall-through 0x57E7E4
        roots = export.patch_roots(["57e7e0 word 1e790008 34000188"])
        self.assertEqual(roots, {0x57E810, 0x57E7E4})
        self.assertEqual(export.patch_roots(["9e8d84 word 1a9f0501 52800021"]), set())

    def test_hash_changes_with_patches(self):
        w = fixture()
        patches = Path(tempfile.mkdtemp())
        before = export.input_hash(w.inputs, w.roots_dir, w.lifter_files, patches)
        (patches / "fps60.patches").write_text("main 0x10 word 0x1 0x2\n")
        self.assertNotEqual(before, export.input_hash(w.inputs, w.roots_dir, w.lifter_files, patches))
