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
            '#define MARAUDER_VERSION "v1.15.0-mobile.5"',
            CONFIG_SOURCE,
        )

    def test_android_capacity_is_reported_separately_from_the_spool(self):
        for expected in (
            'operation == "host"',
            'SD:STATUS:backing=',
            'SD:STATUS:spool_total=',
            'SD:STATUS:spool_free=',
            'androidHostCapacityValid ? this->androidHostTotalBytes',
            'this->card_sz = String(this->cardSizeMB)',
        ):
            self.assertIn(expected, SD_SOURCE)

    def test_capture_ram_is_not_cleared_after_a_failed_spool_write(self):
        buffer_source = (PROJECT_DIR / "esp32_marauder/Buffer.cpp").read_text(
            encoding="utf-8"
        )
        for expected in (
            "FFat.freeBytes() < pending + SPOOL_WRITE_RESERVE_BYTES",
            "bool saved = this->fs == NULL;",
            "if (!saved)",
            "retaining capture data in RAM",
        ):
            self.assertIn(expected, buffer_source)
        self.assertLess(
            buffer_source.index("if (!saved)"),
            buffer_source.index("bufSizeA = 0;", buffer_source.index("void Buffer::save()")),
        )


if __name__ == "__main__":
    unittest.main()
