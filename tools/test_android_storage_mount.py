from pathlib import Path
import unittest


PROJECT_DIR = Path(__file__).resolve().parents[1]
SD_SOURCE = (PROJECT_DIR / "esp32_marauder/SDInterface.cpp").read_text(encoding="utf-8")
CONFIG_SOURCE = (PROJECT_DIR / "esp32_marauder/configs.h").read_text(encoding="utf-8")
BLE_SOURCE = (PROJECT_DIR / "esp32_marauder/MarauderBleSerial.cpp").read_text(
    encoding="utf-8"
)
WIFI_SOURCE = (PROJECT_DIR / "esp32_marauder/WiFiScan.cpp").read_text(
    encoding="utf-8"
)


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
            '#define MARAUDER_VERSION "v1.15.0-mobile.6"',
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

    def test_phone_uart_does_not_pull_nimble_into_other_board_builds(self):
        guarded = BLE_SOURCE.strip()
        self.assertTrue(guarded.startswith("#ifdef MARAUDER_HELTEC_V4"))
        self.assertTrue(guarded.endswith("#endif // MARAUDER_HELTEC_V4"))
        self.assertLess(
            guarded.index("#ifdef MARAUDER_HELTEC_V4"),
            guarded.index("#include <NimBLEDevice.h>"),
        )

    def test_nimble_two_initialization_api_is_heltec_only(self):
        marker = "const bool configureScanCache = !NimBLEDevice::getInitialized();"
        self.assertIn(marker, WIFI_SOURCE)
        guarded_region = WIFI_SOURCE[
            WIFI_SOURCE.rfind("#ifdef MARAUDER_HELTEC_V4", 0, WIFI_SOURCE.index(marker)) :
            WIFI_SOURCE.index("#endif", WIFI_SOURCE.index(marker))
        ]
        self.assertIn(marker, guarded_region)
        self.assertIn("const bool configureScanCache = true;", guarded_region)


if __name__ == "__main__":
    unittest.main()
