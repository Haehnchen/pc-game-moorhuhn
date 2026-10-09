"""Check package contents and reject stale source media."""

import importlib.util
from pathlib import Path
import shutil
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("asset_packer", ROOT / "tools/pack_assets.py")
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)


class PackTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="moorhuhn-pack-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "assets"
        shutil.copytree(ROOT / "tests/fixtures/assets/loader", self.root)
        self.manifest = self.root / "manifest.txt"

    def mutate(self, change):
        document = packer.read_manifest(self.manifest)
        change(document)
        self.manifest.write_text(packer.manifest_text(document))

    def test_categories_and_payloads(self):
        groups = packer.collect_files(self.manifest)
        self.assertEqual([len(groups[name]) for name in ("images", "audio")], [3, 1])
        for files in groups.values():
            data = packer.pack_files(files)
            self.assertEqual(data[:8], b"MHASSETS")
            count, index_size = struct.unpack_from("<II", data, 8)
            self.assertEqual(count, len(files))
            self.assertEqual(data[16 + index_size :], b"".join(content for _, content in files))
            self.assertEqual([path for path, _ in files], sorted(path for path, _ in files))

    def test_corrupt_input_is_rejected(self):
        path = self.root / "audio/typo22.mp3"
        path.write_bytes(path.read_bytes() + b"corrupt")
        with self.assertRaisesRegex(ValueError, "Byte count mismatch: audio/typo22.mp3"):
            packer.collect_files(self.manifest)

    def test_unsafe_and_duplicate_paths_are_rejected(self):
        self.mutate(lambda doc: doc["audio"][0].update(path="../typo22.mp3"))
        with self.assertRaisesRegex(ValueError, "Unsafe asset path"):
            packer.collect_files(self.manifest)
        self.mutate(lambda doc: doc["audio"][0].update(path="tables.txt"))
        with self.assertRaisesRegex(ValueError, "expected audio path"):
            packer.collect_files(self.manifest)

    def test_external_symlink_is_rejected(self):
        audio = self.root / "audio/typo22.mp3"
        outside = self.root.parent / "outside.mp3"
        shutil.copyfile(audio, outside)
        audio.unlink()
        audio.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "escapes root"):
            packer.collect_files(self.manifest)


if __name__ == "__main__":
    unittest.main()
