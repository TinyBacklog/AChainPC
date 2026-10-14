import json
import struct
import tempfile
import unittest
from pathlib import Path

from acre import roots

NOP = b"\x1f\x20\x03\xd5"
LAYOUT = {"text_vaddr": 0, "rodata_vaddr": 0x1000, "data_vaddr": 0x2000}
REAL_INPUTS = Path("<suyu-v0.0.4>/artifacts/aot-validation/effective-inputs")


def write(d, text, rodata=b"", data=b""):
    (d / "data").mkdir(parents=True)
    for name, content in (("text", text), ("rodata", rodata), ("data", data)):
        (d / "data" / f"{name}.bin").write_bytes(content)


class RootsTest(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())

    def test_code_pointer_in_data_becomes_root(self):
        write(self.d, NOP * 64, rodata=struct.pack("<Q", 0x40), data=struct.pack("<Q", 0x2000))
        found, _ = roots.discover(self.d, LAYOUT)
        self.assertIn(0x40, found)
        self.assertNotIn(0x2000, found)  # points outside .text

    def test_unaligned_pointer_ignored(self):
        write(self.d, NOP * 64, rodata=struct.pack("<Q", 0x42))
        found, _ = roots.discover(self.d, LAYOUT)
        self.assertNotIn(0x42, found)

    def test_real_module_has_entry_among_roots(self):
        src = REAL_INPUTS / "rtld"
        if not src.exists():
            self.skipTest("real inputs not on this machine")
        found, entry = roots.discover(src, json.loads((src / "module.json").read_text()))
        self.assertGreater(len(found), 10)
        self.assertIn(entry, found)


if __name__ == "__main__":
    unittest.main()
