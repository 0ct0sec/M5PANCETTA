import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "build_release.py"
SPEC = importlib.util.spec_from_file_location("build_release", SCRIPT)
assert SPEC and SPEC.loader
build_release = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = build_release
SPEC.loader.exec_module(build_release)


class BuildReleaseTests(unittest.TestCase):
    def test_versions_are_read_from_the_selected_environment(self):
        content = build_release.read_platformio_ini(ROOT)
        self.assertEqual(
            build_release.get_version("m5stack-core2", content),
            "0.1.0-core",
        )
        self.assertEqual(
            build_release.get_version("m5stack-cores3se", content),
            "0.1.0-cores3se",
        )
        self.assertEqual(
            build_release.get_hamlet_define_version(
                "m5stack-core2", content
            ),
            "0.1.0-core",
        )
        self.assertEqual(
            build_release.get_hamlet_define_version(
                "m5stack-cores3se", content
            ),
            "0.1.0-cores3se",
        )

    def test_version_metadata_must_match_compiler_define(self):
        content = """
[env:broken]
custom_version = 1.2.3
build_flags =
    -DHAMLET_VERSION='"1.2.4"'
"""
        with self.assertRaisesRegex(ValueError, "Version mismatch"):
            build_release.get_version("broken", content)

    def test_target_flash_geometry_is_not_shared(self):
        core2 = build_release.TARGETS["core2"]
        cores3 = build_release.TARGETS["cores3se"]
        self.assertEqual(core2.chip, "esp32")
        self.assertEqual(core2.flash_mode, "dio")
        self.assertEqual(core2.bootloader_offset, "0x1000")
        self.assertEqual(cores3.chip, "esp32s3")
        self.assertEqual(cores3.flash_mode, "keep")
        self.assertEqual(cores3.bootloader_offset, "0x0")

    def test_merge_verification_rejects_rewritten_bootloader_header(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            bootloader = root / "bootloader.bin"
            app = root / "firmware.bin"
            merged = root / "merged.bin"

            bootloader.write_bytes(b"\xE9\x03\x02\x4F" + b"B" * 60)
            app.write_bytes(b"\xE9" + b"A" * 63)
            merged_data = bytearray(b"\xFF" * (0x10000 + len(app.read_bytes())))
            merged_data[: bootloader.stat().st_size] = bootloader.read_bytes()
            merged_data[0x10000 :] = app.read_bytes()
            merged.write_bytes(merged_data)

            components = [("0x0", bootloader), ("0x10000", app)]
            build_release.verify_release_images(
                app, merged, "0x10000", components
            )

            merged_data[2] = 0x00
            merged.write_bytes(merged_data)
            with self.assertRaisesRegex(ValueError, "altered component"):
                build_release.verify_release_images(
                    app, merged, "0x10000", components
                )

    def test_all_builds_primary_then_compatibility_target(self):
        selected = list(build_release.selected_targets("all"))
        self.assertEqual(
            [target.key for target in selected],
            ["cores3se", "core2"],
        )

    def test_missing_environment_is_rejected(self):
        with self.assertRaises(ValueError):
            build_release.get_env_section(
                "[env:m5stack-core2]\nfoo=bar\n",
                "missing",
            )


if __name__ == "__main__":
    unittest.main()
