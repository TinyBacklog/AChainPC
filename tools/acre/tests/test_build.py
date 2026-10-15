import tempfile
import unittest
from pathlib import Path
from unittest import mock

from acre import build, config


def fake_work() -> Path:
    work = Path(tempfile.mkdtemp())
    (work / "export").mkdir()
    (work / "export" / "recomp_registration.c").write_text("")
    return work


class BuildCompilerTest(unittest.TestCase):
    def test_lifted_c_is_compiled_with_clang(self):
        work = fake_work()
        with mock.patch("shutil.which", side_effect=lambda tool: f"/opt/bin/{tool}"), \
             mock.patch("subprocess.run") as fake:
            fake.return_value.returncode = 0
            build.run(work=work)
        configure = fake.call_args_list[0].args[0]
        self.assertIn("-DCMAKE_C_COMPILER=/opt/bin/clang", configure)
        self.assertIn("-DCMAKE_CXX_COMPILER=/opt/bin/c++", configure)

    def test_missing_clang_stops_before_cmake(self):
        work = fake_work()
        which = lambda tool: None if tool == "clang" else f"/opt/bin/{tool}"
        with mock.patch("shutil.which", side_effect=which), \
             mock.patch("subprocess.run") as fake:
            with self.assertRaises(SystemExit) as e:
                build.run(work=work)
        self.assertEqual(e.exception.code, config.EXIT_NO_COMPILER)
        fake.assert_not_called()

    def configure_with_cache(self, cache: str) -> list[str]:
        work = fake_work()
        build_dir = Path(tempfile.mkdtemp())
        (build_dir / "CMakeCache.txt").write_text(cache)
        with mock.patch("shutil.which", side_effect=lambda tool: f"/opt/bin/{tool}"), \
             mock.patch.object(build, "BUILD_DIR", build_dir), \
             mock.patch("subprocess.run") as fake:
            fake.return_value.returncode = 0
            build.run(work=work)
        return fake.call_args_list[0].args[0]

    def test_compiler_change_reconfigures_fresh(self):
        # CMake drops every -D option when it notices a new compiler in an old
        # cache, so the configure must start fresh instead.
        configure = self.configure_with_cache(
            "CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\nCMAKE_CXX_COMPILER:STRING=/opt/bin/c++\n")
        self.assertIn("--fresh", configure)

    def test_same_compilers_keep_the_cache(self):
        configure = self.configure_with_cache(
            "CMAKE_C_COMPILER:FILEPATH=/opt/bin/clang\nCMAKE_CXX_COMPILER:FILEPATH=/opt/bin/c++\n")
        self.assertNotIn("--fresh", configure)


if __name__ == "__main__":
    unittest.main()
