from pathlib import Path
import unittest


PROJECT_DIR = Path(__file__).resolve().parents[1]
SOURCE = (PROJECT_DIR / "esp32_marauder/esp32_marauder.ino").read_text(encoding="utf-8")


class BootPowerGuardContractTest(unittest.TestCase):
    def test_low_voltage_resets_delay_peripheral_startup(self):
        for expected in (
            "if (reason == ESP_RST_BROWNOUT) guardMs = 2500;",
            "else if (reason == ESP_RST_POWERON) guardMs = 1200;",
            "digitalWrite(kBootGuardVextPin, HIGH);",
            "digitalWrite(kBootGuardGpsPowerPin, HIGH);",
            "delay(guardMs);",
        ):
            self.assertIn(expected, SOURCE)

    def test_guard_runs_before_ui_and_radio_initialization(self):
        guard_call = SOURCE.index("runBootPowerGuard();")
        ui_start = SOURCE.index("heltec_ui_obj.begin();")
        self.assertLess(guard_call, ui_start)

    def test_boot_diagnostic_reports_reset_reason_and_delay(self):
        self.assertIn('"[BOOT] reset=%s (%d), power guard=%lu ms\\n"', SOURCE)


if __name__ == "__main__":
    unittest.main()
