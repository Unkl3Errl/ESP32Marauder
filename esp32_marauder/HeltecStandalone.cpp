#include "HeltecStandalone.h"

#ifdef HAS_HELTEC_STANDALONE

#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include "GpsInterface.h"
#include "WiFiScan.h"

extern WiFiScan wifi_scan_obj;
extern GpsInterface gps_obj;
extern LinkedList<AccessPoint>* access_points;
extern LinkedList<BleDevice>* ble_devices;
extern LinkedList<AirTag>* airtags;
extern LinkedList<Flipper>* flippers;
extern int num_beacon;
extern int num_deauth;
extern int num_probe;
extern int num_eapol;

namespace {
constexpr uint8_t kButtonPin = 0;
constexpr uint8_t kVextPin = 36;
constexpr uint8_t kOledResetPin = 21;
constexpr uint8_t kOledSdaPin = 17;
constexpr uint8_t kOledSclPin = 18;
constexpr uint8_t kGpsPowerPin = 34;
constexpr uint8_t kGpsWakePin = 40;
constexpr uint8_t kGpsResetPin = 42;
constexpr uint8_t kBatteryAdcPin = 1;
constexpr uint8_t kBatteryMeasurePin = 37;

constexpr uint32_t kDebounceMs = 30;
constexpr uint32_t kClickWindowMs = 550;
constexpr uint32_t kLongPressMs = 900;

Preferences display_preferences;

enum SpecialAction : int16_t {
  kOpenWiFi = -1,
  kOpenBluetooth = -2,
  kOpenGps = -3,
  kOpenTransmit = -4,
  kOpenSystem = -5,
  kShowInfo = -6,
  kShowHelp = -7,
  kStopRadios = -8,
  kClearFindings = -9,
  kBack = -10,
  kOpenDisplay = -11,
  kSleep = -12,
  kPowerDown = -13,
  kTimeoutOff = -14,
  kTimeout15 = -15,
  kTimeout30 = -16,
  kTimeout45 = -17,
  kTimeout60 = -18,
};

struct MenuItem {
  const char* label;
  int16_t action;
  bool requires_confirmation;
};

constexpr MenuItem kRootMenu[] = {
    {"WiFi tools", kOpenWiFi, false},
    {"Bluetooth", kOpenBluetooth, false},
    {"GPS", kOpenGps, false},
    {"Transmit tests", kOpenTransmit, false},
    {"System", kOpenSystem, false},
};

constexpr MenuItem kWiFiMenu[] = {
    {"Access points", WIFI_SCAN_AP, false},
    {"AP + stations", WIFI_SCAN_AP_STA, false},
    {"Probe requests", WIFI_SCAN_PROBE, false},
    {"Deauth detect", WIFI_SCAN_DEAUTH, false},
    {"EAPOL detect", WIFI_SCAN_EAPOL, false},
    {"Raw frame stats", WIFI_SCAN_RAW_CAPTURE, false},
    {"Channel use", WIFI_SCAN_CHAN_ACT, false},
    {"Pineapple detect", WIFI_SCAN_PINESCAN, false},
    {"Multi-SSID detect", WIFI_SCAN_MULTISSID, false},
    {"Back", kBack, false},
};

constexpr MenuItem kBluetoothMenu[] = {
    {"BLE devices", BT_SCAN_ALL, false},
    {"Skimmer detect", BT_SCAN_SKIMMERS, false},
    {"AirTag scan", BT_SCAN_AIRTAG, false},
    {"Flipper scan", BT_SCAN_FLIPPER, false},
    {"Meta/Ray-Ban", BT_SCAN_RAYBAN, false},
    {"BLE analyzer", BT_SCAN_ANALYZER, false},
    {"Back", kBack, false},
};

constexpr MenuItem kGpsMenu[] = {
    {"GPS status", WIFI_SCAN_GPS_DATA, false},
    {"NMEA monitor", WIFI_SCAN_GPS_NMEA, false},
    {"GPS tracker", GPS_TRACKER, false},
    {"Wardrive", WIFI_SCAN_WAR_DRIVE, false},
    {"Log POI", GPS_POI, false},
    {"Back", kBack, false},
};

// Transmit-capable entries require a second explicit long-press selection on
// a warning screen. They are never started during boot or passive validation.
constexpr MenuItem kTransmitMenu[] = {
    {"Random beacons", WIFI_ATTACK_BEACON_SPAM, true},
    {"Rickroll beacons", WIFI_ATTACK_RICK_ROLL, true},
    {"Funny beacons", WIFI_ATTACK_FUNNY_BEACON, true},
    {"SwiftPair BLE", BT_ATTACK_SWIFTPAIR_SPAM, true},
    {"Back", kBack, false},
};

constexpr MenuItem kSystemMenu[] = {
    {"Stop radios", kStopRadios, false},
    {"Clear findings", kClearFindings, false},
    {"Display timeout", kOpenDisplay, false},
    {"Device info", kShowInfo, false},
    {"Button help", kShowHelp, false},
    {"Sleep (PRG wake)", kSleep, true},
    {"Power down", kPowerDown, true},
    {"Back", kBack, false},
};

constexpr MenuItem kDisplayMenu[] = {
    {"Always on", kTimeoutOff, false},
    {"Turn off after 15s", kTimeout15, false},
    {"Turn off after 30s", kTimeout30, false},
    {"Turn off after 45s", kTimeout45, false},
    {"Turn off after 60s", kTimeout60, false},
    {"Back", kBack, false},
};

template <size_t N>
constexpr uint8_t itemCount(const MenuItem (&)[N]) {
  return static_cast<uint8_t>(N);
}

const MenuItem* menuFor(HeltecStandalone::Screen screen) {
  switch (screen) {
    case HeltecStandalone::Screen::Root:
      return kRootMenu;
    case HeltecStandalone::Screen::WiFi:
      return kWiFiMenu;
    case HeltecStandalone::Screen::Bluetooth:
      return kBluetoothMenu;
    case HeltecStandalone::Screen::GPS:
      return kGpsMenu;
    case HeltecStandalone::Screen::Transmit:
      return kTransmitMenu;
    case HeltecStandalone::Screen::System:
      return kSystemMenu;
    case HeltecStandalone::Screen::Display:
      return kDisplayMenu;
    default:
      return nullptr;
  }
}
}  // namespace

void HeltecStandalone::begin() {
  pinMode(kButtonPin, INPUT_PULLUP);

  display_preferences.begin("heltec-ui", false);
  screen_timeout_seconds = display_preferences.getUChar("screenOff", 30);
  if (screen_timeout_seconds != 0 && screen_timeout_seconds != 15 &&
      screen_timeout_seconds != 30 && screen_timeout_seconds != 45 &&
      screen_timeout_seconds != 60) {
    screen_timeout_seconds = 30;
  }

  // The Heltec OLED is supplied by Vext, which is active low.
  pinMode(kVextPin, OUTPUT);
  digitalWrite(kVextPin, LOW);
  delay(20);

  pinMode(kOledResetPin, OUTPUT);
  digitalWrite(kOledResetPin, LOW);
  delay(10);
  digitalWrite(kOledResetPin, HIGH);
  delay(10);

  Wire.begin(kOledSdaPin, kOledSclPin);
  oled.setI2CAddress(0x3C << 1);
  oled.setBusClock(400000);
  oled.begin();
  oled.setFontMode(1);

  // Power and release the optional GPS module before Marauder probes UART1.
  pinMode(kGpsPowerPin, OUTPUT);
  digitalWrite(kGpsPowerPin, LOW);
  pinMode(kGpsResetPin, OUTPUT);
  digitalWrite(kGpsResetPin, HIGH);
  pinMode(kGpsWakePin, OUTPUT);
  digitalWrite(kGpsWakePin, HIGH);

  pinMode(kBatteryMeasurePin, OUTPUT);
  digitalWrite(kBatteryMeasurePin, LOW);
  analogSetPinAttenuation(kBatteryAdcPin, ADC_11db);

  raw_pressed = digitalRead(kButtonPin) == LOW;
  stable_pressed = raw_pressed;
  raw_changed_at = millis();
  last_interaction = raw_changed_at;
  screen = Screen::Boot;
  setBootStatus("OLED ready");
  Serial.println(F("[Heltec V4] OLED and single-button UI initialized"));
}

void HeltecStandalone::setBootStatus(const char* status) {
  if (screen != Screen::Boot) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 10, "MARAUDER / HELTEC V4");
  oled.drawHLine(0, 13, 128);
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 29, "Starting firmware...");
  oled.drawStr(0, 43, status ? status : "");
  oled.sendBuffer();
}

void HeltecStandalone::ready() {
  screen = Screen::Root;
  selected = 0;
  notice = "Ready";
  notice_until = millis() + 1200;
  render();
}

void HeltecStandalone::main(uint32_t now) {
  pollButton(now);

  // Gesture dispatch can render synchronously and record an interaction using
  // a newer millis() value than the timestamp supplied by loop(). Refresh the
  // clock before calculating idle time; otherwise unsigned subtraction treats
  // that future interaction as almost 49 days of inactivity and blanks the
  // OLED immediately after the menu selection changes.
  now = millis();

  // Never blank between the physical edge and the completed single/double-click
  // gesture. This also prevents the timeout from winning the debounce window.
  const bool interaction_in_progress =
      raw_pressed || stable_pressed || click_count > 0;
  if (!screen_blanked && screen_timeout_seconds > 0 &&
      !interaction_in_progress &&
      now - last_interaction >= static_cast<uint32_t>(screen_timeout_seconds) * 1000UL) {
    blankScreen();
  }
  if (screen_blanked) return;

  const uint32_t interval = screen == Screen::Running ? 500 : 150;
  if (now - last_render >= interval) {
    last_render = now;
    render();
  }
}

void HeltecStandalone::pollButton(uint32_t now) {
  const bool pressed = digitalRead(kButtonPin) == LOW;
  if (pressed != raw_pressed) {
    raw_pressed = pressed;
    raw_changed_at = now;
    // A raw edge is user activity even before it survives debounce. Without
    // this update, a press arriving at the timeout boundary can lose to the
    // blanking check in main().
    last_interaction = now;
  }

  if (now - raw_changed_at >= kDebounceMs && stable_pressed != raw_pressed) {
    stable_pressed = raw_pressed;
    if (stable_pressed) {
      last_interaction = now;
      if (screen_blanked) {
        wake_press = true;
        long_dispatched = true;
        click_count = 0;
        wakeScreen(now);
        return;
      }
      pressed_at = now;
      long_dispatched = false;
    } else {
      if (wake_press) {
        wake_press = false;
        long_dispatched = false;
        click_count = 0;
        return;
      }
      const uint32_t duration = now - pressed_at;
      if (!long_dispatched) {
        if (duration >= kLongPressMs) {
          click_count = 0;
          dispatch(Gesture::Long);
        } else {
          if (click_count < 2) ++click_count;
          click_deadline = now + kClickWindowMs;
        }
      }
    }
  }

  if (stable_pressed && !wake_press && !long_dispatched &&
      now - pressed_at >= kLongPressMs) {
    long_dispatched = true;
    click_count = 0;
    dispatch(Gesture::Long);
  }

  // A second physical press can begin just before the click deadline while its
  // debounced state is still false. Do not finalize the pending click until
  // both the raw and stable inputs are released, or that second tap can be
  // misclassified as a single.
  if (!raw_pressed && !stable_pressed && click_count > 0 && now >= click_deadline) {
    const uint8_t completed = click_count;
    click_count = 0;
    if (completed == 1)
      dispatch(Gesture::Single);
    else
      dispatch(Gesture::Double);
  }
}

void HeltecStandalone::dispatch(Gesture gesture) {
  last_interaction = millis();
  switch (gesture) {
    case Gesture::Single:
      nextItem();
      break;
    case Gesture::Double:
      goBack(false);
      break;
    case Gesture::Long:
      selectItem();
      break;
  }
  render();
  // Some scan initializers are synchronous. Measure inactivity from when the
  // requested action and its first render finish, not from before dispatch.
  last_interaction = millis();
}

void HeltecStandalone::nextItem() {
  if (screen == Screen::Confirm) {
    goBack(false);
    return;
  }
  if (screen == Screen::Running || screen == Screen::Info || screen == Screen::Help) return;
  const uint8_t count = menuSize();
  if (count) selected = (selected + 1) % count;
}

void HeltecStandalone::selectItem() {
  if (screen == Screen::Running) return;
  if (screen == Screen::Info || screen == Screen::Help) {
    screen = Screen::System;
    selected = 0;
    return;
  }
  if (screen == Screen::Confirm) {
    const int16_t confirmed_action = pending_action;
    if (confirmed_action == kSleep) enterDeepSleep(true);
    if (confirmed_action == kPowerDown) enterDeepSleep(false);
    startAction(confirmed_action, pending_label);
    return;
  }

  const MenuItem* menu = menuFor(screen);
  if (!menu || selected >= menuSize()) return;
  const MenuItem& item = menu[selected];

  if (item.requires_confirmation) {
    pending_action = item.action;
    pending_label = item.label;
    return_screen = screen;
    screen = Screen::Confirm;
    return;
  }

  switch (item.action) {
    case kOpenWiFi:
      screen = Screen::WiFi;
      break;
    case kOpenBluetooth:
      screen = Screen::Bluetooth;
      break;
    case kOpenGps:
      screen = Screen::GPS;
      break;
    case kOpenTransmit:
      screen = Screen::Transmit;
      break;
    case kOpenSystem:
      screen = Screen::System;
      break;
    case kOpenDisplay:
      screen = Screen::Display;
      break;
    case kShowInfo:
      screen = Screen::Info;
      break;
    case kShowHelp:
      screen = Screen::Help;
      break;
    case kStopRadios:
      stopAction();
      notice = "Radios stopped";
      notice_until = millis() + 1500;
      break;
    case kClearFindings:
      wifi_scan_obj.RunClearAPs();
      wifi_scan_obj.RunClearStations();
      wifi_scan_obj.clearList(CLEAR_BLE);
      wifi_scan_obj.clearList(CLEAR_AT);
      notice = "Findings cleared";
      notice_until = millis() + 1500;
      break;
    case kTimeoutOff:
      setScreenTimeout(0);
      break;
    case kTimeout15:
      setScreenTimeout(15);
      break;
    case kTimeout30:
      setScreenTimeout(30);
      break;
    case kTimeout45:
      setScreenTimeout(45);
      break;
    case kTimeout60:
      setScreenTimeout(60);
      break;
    case kBack:
      goBack(false);
      return;
    default:
      return_screen = screen;
      startAction(item.action, item.label);
      return;
  }
  selected = 0;
}

void HeltecStandalone::goBack(bool home) {
  if (screen == Screen::Running) {
    stopAction();
    screen = home ? Screen::Root : return_screen;
  } else if (screen == Screen::Confirm) {
    screen = home ? Screen::Root : return_screen;
  } else if (screen == Screen::Info || screen == Screen::Help) {
    screen = home ? Screen::Root : Screen::System;
  } else if (screen == Screen::Display) {
    screen = home ? Screen::Root : Screen::System;
  } else if (screen != Screen::Root) {
    screen = Screen::Root;
  }
  selected = 0;
}

void HeltecStandalone::startAction(int16_t action, const char* label) {
  stopAction();

  num_beacon = 0;
  num_probe = 0;
  num_deauth = 0;
  num_eapol = 0;
  wifi_scan_obj.mgmt_frames = 0;
  wifi_scan_obj.data_frames = 0;
  wifi_scan_obj.beacon_frames = 0;
  wifi_scan_obj.deauth_frames = 0;
  wifi_scan_obj.eapol_frames = 0;
  wifi_scan_obj.bt_frames = 0;

  wifi_scan_obj.StartScan(static_cast<uint8_t>(action));
  running_label = label;
  running_since = millis();
  screen = Screen::Running;
}

void HeltecStandalone::stopAction() {
  wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
}

void HeltecStandalone::setScreenTimeout(uint8_t seconds) {
  screen_timeout_seconds = seconds;
  display_preferences.putUChar("screenOff", seconds);
  last_interaction = millis();
  notice = seconds == 0 ? "Screen always on" : "Screen off after " + String(seconds) + "s";
  notice_until = last_interaction + 1800;
}

void HeltecStandalone::blankScreen() {
  oled.setPowerSave(1);
  screen_blanked = true;
}

void HeltecStandalone::wakeScreen(uint32_t now) {
  screen_blanked = false;
  last_interaction = now;
  oled.setPowerSave(0);
  render();
}

[[noreturn]] void HeltecStandalone::enterDeepSleep(bool wake_on_button) {
  stopAction();

  oled.clearBuffer();
  drawHeader(wake_on_button ? "SLEEP" : "POWER DOWN");
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 28, wake_on_button ? "Press PRG to wake" : "Use RST or power cycle");
  oled.drawStr(0, 42, "Stopping radios + GPS");
  oled.drawStr(0, 56, "Entering deep sleep...");
  oled.sendBuffer();

  wifi_scan_obj.shutdownWiFi();
  wifi_scan_obj.shutdownBLE();
  digitalWrite(kGpsWakePin, LOW);
  digitalWrite(kGpsResetPin, LOW);
  digitalWrite(kGpsPowerPin, HIGH);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (wake_on_button) {
    while (digitalRead(kButtonPin) == LOW) delay(10);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  }

  Serial.println(wake_on_button ? F("[Heltec V4] Sleeping; PRG wakes the device")
                                : F("[Heltec V4] Powered down; RST or power cycle required"));
  Serial.flush();
  delay(250);
  oled.setPowerSave(1);
  digitalWrite(kVextPin, HIGH);
  esp_deep_sleep_start();
  __builtin_unreachable();
}

void HeltecStandalone::render() {
  if (screen == Screen::Boot) return;
  switch (screen) {
    case Screen::Running:
      renderRunning();
      break;
    case Screen::Confirm:
      renderConfirm();
      break;
    case Screen::Info:
      renderInfo();
      break;
    case Screen::Help:
      renderHelp();
      break;
    default:
      renderMenu();
      break;
  }
}

void HeltecStandalone::renderMenu() {
  oled.clearBuffer();
  drawHeader(menuTitle());

  const MenuItem* menu = menuFor(screen);
  const uint8_t count = menuSize();
  if (menu && count) {
    uint8_t first = selected > 2 ? selected - 2 : 0;
    if (count > 4 && first > count - 4) first = count - 4;
    for (uint8_t row = 0; row < 4 && first + row < count; ++row) {
      drawLine(row, menu[first + row].label, first + row == selected);
    }
  }

  oled.setFont(u8g2_font_4x6_tf);
  if (notice.length() && millis() < notice_until)
    oled.drawStr(0, 63, notice.c_str());
  else
    oled.drawStr(0, 63, "1x next 2x back hold select");
  oled.sendBuffer();
}

void HeltecStandalone::renderRunning() {
  oled.clearBuffer();
  drawHeader(running_label ? running_label : scanModeName(wifi_scan_obj.currentScanMode));
  oled.setFont(u8g2_font_5x8_tf);

  const uint32_t elapsed = (millis() - running_since) / 1000;
  String line = "RUN " + String(elapsed) + "s  CH " + String(wifi_scan_obj.set_channel);
  oled.drawStr(0, 24, line.c_str());

  const uint8_t mode = wifi_scan_obj.currentScanMode;
  if (mode == WIFI_SCAN_GPS_DATA || mode == WIFI_SCAN_GPS_NMEA ||
      mode == GPS_TRACKER || mode == GPS_POI || mode == WIFI_SCAN_WAR_DRIVE) {
    line = "GPS " + String(gps_obj.getFixStatus() ? "FIX" : "NO FIX") +
           "  SAT " + String(gps_obj.getNumSats());
    oled.drawStr(0, 35, line.c_str());
    line = gps_obj.getLat() + ", " + gps_obj.getLon();
    if (line.length() > 25) line.remove(25);
    oled.drawStr(0, 46, line.c_str());
    line = "AP " + String(access_points ? access_points->size() : 0) +
           "  POI " + String(wifi_scan_obj.poiCount);
    oled.drawStr(0, 57, line.c_str());
  } else if (mode == BT_SCAN_ALL || mode == BT_SCAN_SKIMMERS ||
             mode == BT_SCAN_AIRTAG || mode == BT_SCAN_FLIPPER ||
             mode == BT_SCAN_RAYBAN || mode == BT_SCAN_ANALYZER) {
    line = "BLE " + String(ble_devices ? ble_devices->size() : 0) +
           "  frames " + String(wifi_scan_obj.bt_frames);
    oled.drawStr(0, 35, line.c_str());
    line = "Tags " + String(airtags ? airtags->size() : 0) +
           "  Flip " + String(flippers ? flippers->size() : 0);
    oled.drawStr(0, 46, line.c_str());
    oled.drawStr(0, 57, "2x: stop / back");
  } else {
    line = "AP " + String(access_points ? access_points->size() : 0) +
           " B " + String(num_beacon) + " P " + String(num_probe);
    oled.drawStr(0, 35, line.c_str());
    line = "D " + String(num_deauth) + " E " + String(num_eapol) +
           " RAW " + String(wifi_scan_obj.mgmt_frames + wifi_scan_obj.data_frames);
    oled.drawStr(0, 46, line.c_str());
    oled.drawStr(0, 57, "2x: stop / back");
  }
  oled.sendBuffer();
}

void HeltecStandalone::renderConfirm() {
  oled.clearBuffer();
  const bool power_action = pending_action == kSleep || pending_action == kPowerDown;
  drawHeader(pending_action == kPowerDown ? "POWER DOWN" :
             pending_action == kSleep ? "SLEEP" : "TRANSMIT WARNING");
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 25, pending_label ? pending_label : "Transmit action");
  oled.drawStr(0, 37, power_action ?
      (pending_action == kSleep ? "PRG wakes + restarts" : "RST/power cycle wakes") :
      "Authorized testing only");
  oled.drawStr(0, 49, "Hold: confirm");
  oled.drawStr(0, 61, "1x / 2x: cancel");
  oled.sendBuffer();
}

void HeltecStandalone::renderInfo() {
  oled.clearBuffer();
  drawHeader("DEVICE INFO");
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 24, "Heltec WiFi LoRa 32 V4");
  String line = String("Marauder ") + MARAUDER_VERSION;
  oled.drawStr(0, 35, line.c_str());
  const uint16_t battery_mv = readBatteryMillivolts();
  line = battery_mv > 2500 ? "Battery " + String(battery_mv) + " mV" : "Battery: USB/no cell";
  oled.drawStr(0, 46, line.c_str());
  line = "GPS " + String(gps_obj.getGpsModuleStatus() ? "online" : "not found");
  oled.drawStr(0, 57, line.c_str());
  oled.sendBuffer();
}

void HeltecStandalone::renderHelp() {
  oled.clearBuffer();
  drawHeader("PRG BUTTON");
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(0, 23, "1x      next item");
  oled.drawStr(0, 33, "2x      back / stop");
  oled.drawStr(0, 43, "Long    select / start");
  oled.drawStr(0, 53, "Long press = 0.9 sec");
  oled.drawStr(0, 63, "First press wakes screen");
  oled.sendBuffer();
}

void HeltecStandalone::drawHeader(const char* title) {
  oled.setFont(u8g2_font_6x10_tf);
  String clipped = title ? title : "MARAUDER";
  if (clipped.length() > 21) clipped.remove(21);
  oled.drawStr(0, 10, clipped.c_str());
  oled.drawHLine(0, 12, 128);
}

void HeltecStandalone::drawLine(uint8_t row, const String& value, bool selected_line) {
  const uint8_t y = 16 + row * 10;
  String clipped = value;
  if (clipped.length() > 20) clipped.remove(20);
  oled.setFont(u8g2_font_5x8_tf);
  if (selected_line) {
    oled.drawBox(0, y - 1, 128, 9);
    oled.setDrawColor(0);
    oled.drawStr(2, y + 7, (String("> ") + clipped).c_str());
    oled.setDrawColor(1);
  } else {
    oled.drawStr(2, y + 7, (String("  ") + clipped).c_str());
  }
}

uint8_t HeltecStandalone::menuSize() const {
  switch (screen) {
    case Screen::Root:
      return itemCount(kRootMenu);
    case Screen::WiFi:
      return itemCount(kWiFiMenu);
    case Screen::Bluetooth:
      return itemCount(kBluetoothMenu);
    case Screen::GPS:
      return itemCount(kGpsMenu);
    case Screen::Transmit:
      return itemCount(kTransmitMenu);
    case Screen::System:
      return itemCount(kSystemMenu);
    case Screen::Display:
      return itemCount(kDisplayMenu);
    default:
      return 0;
  }
}

const char* HeltecStandalone::menuTitle() const {
  switch (screen) {
    case Screen::Root:
      return "MARAUDER / HELTEC";
    case Screen::WiFi:
      return "WIFI TOOLS";
    case Screen::Bluetooth:
      return "BLUETOOTH";
    case Screen::GPS:
      return "GPS";
    case Screen::Transmit:
      return "TRANSMIT TESTS";
    case Screen::System:
      return "SYSTEM";
    case Screen::Display:
      return "DISPLAY TIMEOUT";
    default:
      return "MARAUDER";
  }
}

uint16_t HeltecStandalone::readBatteryMillivolts() {
  digitalWrite(kBatteryMeasurePin, HIGH);
  delay(3);
  const uint32_t divider_mv = analogReadMilliVolts(kBatteryAdcPin);
  digitalWrite(kBatteryMeasurePin, LOW);
  return static_cast<uint16_t>((divider_mv * 49UL) / 10UL);
}

const char* HeltecStandalone::scanModeName(uint8_t mode) const {
  switch (mode) {
    case WIFI_SCAN_OFF:
      return "Idle";
    case WIFI_SCAN_AP:
      return "Access points";
    case WIFI_SCAN_AP_STA:
      return "AP + stations";
    case WIFI_SCAN_PROBE:
      return "Probe requests";
    case WIFI_SCAN_DEAUTH:
      return "Deauth detect";
    case WIFI_SCAN_EAPOL:
      return "EAPOL detect";
    case BT_SCAN_ALL:
      return "BLE devices";
    case WIFI_SCAN_WAR_DRIVE:
      return "Wardrive";
    default:
      return "Marauder running";
  }
}

#endif
