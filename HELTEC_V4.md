# ESP32 Marauder for Heltec WiFi LoRa 32 V4

This target adapts ESP32 Marauder to the Heltec WiFi LoRa 32 V4's built-in
128x64 OLED, PRG/USER button, 16 MB flash, 2 MB PSRAM, and optional GPS port.
It keeps Marauder's serial CLI while adding a compact standalone menu.

## Button controls

- One click: next menu item
- Two clicks: return to the previous menu or stop the running action
- Hold for 0.9 seconds: select, start, or confirm

Clicks are collected for 550 ms so a single click is not acted on before a
possible second click arrives. Holding PRG during reset still invokes
the ESP32-S3 ROM bootloader because GPIO0 is a hardware strap pin.

The System menu includes a persistent display timeout setting: always on, 15,
30, 45, or 60 seconds. The default is 30 seconds. The first PRG press after the
OLED blanks only wakes the screen and cannot activate the highlighted item.

System also provides **Sleep (PRG wake)** and **Power down**. Both stop active
Wi-Fi/BLE work, power down the GPS and OLED rail, and put the ESP32-S3 into deep
sleep. Sleep enables GPIO0 as a wake source, so PRG wakes and restarts the
firmware. Power down leaves no firmware wake source and requires RST or a power
cycle. The board has no software-controlled latch on its main 3.3 V rail, so
Power down is minimum-power deep sleep rather than a physical battery
disconnect.

The physical RST switch is wired directly to `CHIP_PU`; firmware cannot debounce
it or require a long press.

## Standalone menus

The OLED exposes passive Wi-Fi scans, BLE scans, GPS status/tracking, wardriving,
device status, and several transmit test modes. Every transmit-capable OLED item
has a separate warning screen and requires a second deliberate select gesture.
The full upstream command set remains available over USB serial at 115200 baud.

This board has no microSD slot. Captures and wardrive output can be streamed over
serial, but are not retained on removable media.

## Build

PlatformIO requires Python 3.10 through 3.13. If `pio` is not already installed
under a compatible Python, create the local build environment once:

```sh
python3.13 -m venv .platformio-venv
.platformio-venv/bin/python -m pip install platformio==6.1.19 esptool==4.9.0
```

Then, from this directory:

```sh
.platformio-venv/bin/pio run -e heltec_v4
```

To upload through native USB, set the current port explicitly:

```sh
.platformio-venv/bin/pio run -e heltec_v4 -t upload --upload-port /dev/cu.usbmodemXXXX
```

The target does not initialize the SX1262 LoRa radio. It is left dormant; the
firmware uses only the ESP32-S3's 2.4 GHz Wi-Fi/BLE radio.
