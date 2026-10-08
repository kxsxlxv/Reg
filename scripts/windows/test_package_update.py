#!/usr/bin/env python3
"""Offline contract tests for the GitHub-release delta manifest generator."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

PACKAGE_SCRIPT = Path(__file__).with_name("package-update.py")
SPEC = importlib.util.spec_from_file_location("package_update", PACKAGE_SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / "build"
        self.out = Path(self.directory.name) / "dist"
        self.root.mkdir()
        for exe in ("reg_launcher.exe", "reg_probe.exe",
                    "reg_replay.exe", "reg_updater.exe"):
            (self.root / exe).write_bytes(b"executable " + exe.encode())
        (self.root / "SDL3.dll").write_bytes(b"library")
        (self.root / "fonts").mkdir()
        (self.root / "fonts" / "Roboto.ttf").write_bytes(b"font")
        (self.root / "shaders").mkdir()
        (self.root / "shaders" / "video.vert.spv").write_bytes(b"spv")

    def test_manifest_contains_verified_individual_assets(self):
        manifest = MODULE.package(self.root, self.out, "dev", "dev-" + "a" * 40)
        self.assertEqual(manifest["schema_version"], 1)
        names = {entry["path"] for entry in manifest["files"]}
        self.assertIn("Launcher.exe", names)
        self.assertIn("reg_updater.exe", names)
        self.assertNotIn("reg_launcher.exe", names)
        self.assertIn("fonts/Roboto.ttf", names)
        self.assertIn("shaders/video.vert.spv", names)
        for entry in manifest["files"]:
            asset = self.out / "assets" / entry["asset"]
            self.assertTrue(asset.exists())
            self.assertEqual(asset.stat().st_size, entry["size"])
            self.assertEqual(hashlib.sha256(asset.read_bytes()).hexdigest(),
                             entry["sha256"])
        with zipfile.ZipFile(self.out / "portable.zip") as archive:
            self.assertIn("VideoConsole/Launcher.exe", archive.namelist())
            self.assertIn("VideoConsole/manifest.json", archive.namelist())

    def test_rejects_missing_critical_binary(self):
        (self.root / "reg_updater.exe").unlink()
        with self.assertRaises(ValueError):
            MODULE.package(self.root, self.out, "dev", "dev-" + "a" * 40)

    def test_rejects_invalid_channel_or_tag(self):
        with self.assertRaises(ValueError):
            MODULE.package(self.root, self.out, "dev", "v1.2.3")
        with self.assertRaises(ValueError):
            MODULE.package(self.root, self.out, "stable", "dev-" + "a" * 40)

    def test_rejects_missing_assets(self):
        (self.root / "shaders" / "video.vert.spv").unlink()
        with self.assertRaises(ValueError):
            MODULE.package(self.root, self.out, "dev", "dev-" + "a" * 40)


if __name__ == "__main__":
    unittest.main()
