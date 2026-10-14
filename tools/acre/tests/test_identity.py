import hashlib
import tempfile
import unittest
from pathlib import Path

from acre import identity


def make_module(root, name, text=b"t", rodata=b"r", data=b"d"):
    d = root / name / "data"
    d.mkdir(parents=True)
    for seg, content in (("text", text), ("rodata", rodata), ("data", data)):
        (d / f"{seg}.bin").write_bytes(content)


def h(b):
    return hashlib.sha256(b).hexdigest()


class IdentityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.expected = {
            "modules": {"main": [h(b"t"), h(b"r"), h(b"d")]},
            "known_other_versions": {h(b"old")[:12]: "1.0.0"},
        }

    def test_matching_inputs_pass(self):
        make_module(self.tmp, "main")
        identity.verify(self.tmp, self.expected)

    def test_patched_segment_names_module_and_segment(self):
        make_module(self.tmp, "main", text=b"patched")
        with self.assertRaisesRegex(identity.IdentityError, r"main.*text"):
            identity.verify(self.tmp, self.expected)

    def test_known_old_version_says_update_missing(self):
        make_module(self.tmp, "main", text=b"old")
        with self.assertRaisesRegex(identity.IdentityError, r"1\.0\.0.*update"):
            identity.verify(self.tmp, self.expected)

    def test_missing_module(self):
        with self.assertRaisesRegex(identity.IdentityError, r"main.*missing"):
            identity.verify(self.tmp, self.expected)

    def test_shipped_version_file_is_complete(self):
        exp = identity.load_expected(Path(__file__).parents[3] / "game/version.json")
        self.assertEqual(set(exp["modules"]), {"rtld", "main", "sdk"})
        for hashes in exp["modules"].values():
            self.assertEqual(len(hashes), 3)
            self.assertTrue(all(len(x) == 64 for x in hashes))


if __name__ == "__main__":
    unittest.main()
