"""Check that releases resolve bundled libraries from their own folder."""

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("linux_package", ROOT / "tools/packaging/linux_package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.stage = Path("/tmp/relocated game")
        self.executable = self.stage / "moorhuhn"

    def test_folder_with_spaces(self):
        output = (
            "linux-vdso.so.1 (0x1234)\n"
            f"libSDL3.so.0 => {self.stage}/libSDL3.so.0 (0x1234)\n"
            "libc.so.6 => /usr/lib/libc.so.6 (0x1234)\n"
            "/lib64/ld-linux-x86-64.so.2 (0x1234)\n"
        )
        with patch.object(package, "run", return_value=output):
            result = package.dependencies(self.executable, self.stage)
        self.assertEqual(result["libSDL3.so.0"], self.stage / "libSDL3.so.0")

    def test_system_copy_cannot_replace_bundled_library(self):
        with patch.object(package, "run", return_value="libSDL3.so.0 => /usr/lib/libSDL3.so.0 (0x1234)"):
            with self.assertRaisesRegex(ValueError, "does not resolve beside"):
                package.dependencies(self.executable, self.stage)

    def test_missing_library_is_rejected(self):
        with patch.object(package, "run", return_value="libSDL3_mixer.so.0 => not found"):
            with self.assertRaisesRegex(ValueError, "Unresolved ELF dependency"):
                package.dependencies(self.executable, self.stage)

    def test_unbundled_dependency_is_rejected(self):
        with patch.object(package, "run", return_value="libunexpected.so.1 => /usr/lib/libunexpected.so.1 (0x1234)"):
            with self.assertRaisesRegex(ValueError, "Unexpected system runtime"):
                package.dependencies(self.executable, self.stage)


if __name__ == "__main__":
    unittest.main()
