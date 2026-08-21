from pathlib import Path
import unittest


PROJECT_DIR = Path(__file__).resolve().parents[1]
SD_SOURCE = (PROJECT_DIR / "esp32_marauder/SDInterface.cpp").read_text(encoding="utf-8")
CONFIG_SOURCE = (PROJECT_DIR / "esp32_marauder/configs.h").read_text(encoding="utf-8")


class AndroidStorageMountContractTest(unittest.TestCase):
    def test_formats_within_the_initial_mount_call(self):
        self.assertIn('FFat.begin(true, "/android", 10, "android")', SD_SOURCE)
        self.assertNotIn("FFat.format(", SD_SOURCE)

    def test_storage_commands_retry_after_boot_mount_failure(self):
        self.assertIn(
            "if (!this->supported && !this->initSD())",
            SD_SOURCE,
        )
        self.assertIn("if (this->supported) return true;", SD_SOURCE)

    def test_archive_release_still_requires_size_and_crc(self):
        for expected in (
            "actualSize != expectedSize",
            "actualChecksum != expectedChecksum",
            "MARAUDER_STORAGE.remove(path)",
        ):
            self.assertIn(expected, SD_SOURCE)

    def test_version_identifies_the_storage_fix(self):
        self.assertIn(
            '#define MARAUDER_VERSION "v1.15.0-mobile.1"',
            CONFIG_SOURCE,
        )


if __name__ == "__main__":
    unittest.main()
