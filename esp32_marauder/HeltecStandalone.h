#pragma once

#include "configs.h"

#ifdef HAS_HELTEC_STANDALONE

#include <Arduino.h>
#include <U8g2lib.h>

class HeltecStandalone {
 public:
  void begin();
  void setBootStatus(const char* status);
  void ready();
  void main(uint32_t now);

 public:
  enum class Screen : uint8_t {
    Boot,
    Root,
    WiFi,
    Bluetooth,
    GPS,
    Transmit,
    System,
    Display,
    Confirm,
    Running,
    Info,
    Help,
  };

  enum class Gesture : uint8_t {
    Single,
    Double,
    Long,
  };

 private:

  // This target's Arduino variant maps the global Wire bus to GPIO17/18, so
  // U8g2 can safely reinitialize hardware I2C without moving the OLED bus.
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled =
      U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, 21);

  Screen screen = Screen::Boot;
  Screen return_screen = Screen::Root;
  uint8_t selected = 0;
  uint8_t click_count = 0;
  bool raw_pressed = false;
  bool stable_pressed = false;
  bool long_dispatched = false;
  bool wake_press = false;
  bool screen_blanked = false;
  uint8_t screen_timeout_seconds = 30;
  uint32_t raw_changed_at = 0;
  uint32_t pressed_at = 0;
  uint32_t click_deadline = 0;
  uint32_t last_interaction = 0;
  uint32_t last_render = 0;
  uint32_t running_since = 0;
  int16_t pending_action = 0;
  const char* pending_label = nullptr;
  const char* running_label = nullptr;
  String notice;
  uint32_t notice_until = 0;

  void pollButton(uint32_t now);
  void dispatch(Gesture gesture);
  void nextItem();
  void selectItem();
  void goBack(bool home = false);
  void setScreenTimeout(uint8_t seconds);
  void blankScreen();
  void wakeScreen(uint32_t now);
  [[noreturn]] void enterDeepSleep(bool wake_on_button);
  void startAction(int16_t action, const char* label);
  void stopAction();
  void render();
  void renderMenu();
  void renderRunning();
  void renderConfirm();
  void renderInfo();
  void renderHelp();
  void drawHeader(const char* title);
  void drawLine(uint8_t row, const String& value, bool selected_line = false);
  uint8_t menuSize() const;
  const char* menuTitle() const;
  uint16_t readBatteryMillivolts();
  const char* scanModeName(uint8_t mode) const;
};

#endif
