#pragma once

#include <Arduino.h>

class NimBLECharacteristic;
class NimBLEServer;

/**
 * USB CDC plus a phone-facing Nordic UART Service for the Heltec V4 target.
 *
 * Existing Marauder code continues to use `Serial`; configs.h maps that name
 * to this Stream on the mobile target so command input and output work over
 * either USB or BLE without changing the upstream command handlers.
 */
class MarauderBleSerial : public Stream {
public:
  void begin(unsigned long baud);
  explicit operator bool() const;

  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  size_t write(uint8_t value) override;
  size_t write(const uint8_t *buffer, size_t size) override;
  using Print::write;

  void beginBle();
  void prepareForBleDeinit();
  void loop();

  // Called only by the NimBLE callbacks owned by this bridge.
  void receiveBle(const uint8_t *data, size_t size);
  void setBleConnected(bool connected, uint16_t connectionHandle = 0xffff);
  void setBleSubscribed(bool subscribed);
  void setBleMtu(uint16_t mtu);
  bool isBleReady() const;

private:
  enum class InputSource : uint8_t { NONE, USB, BLE };

  static constexpr size_t RX_CAPACITY = 2048;
  static constexpr size_t TX_CAPACITY = 32768;
  static constexpr size_t MAX_NOTIFY_PAYLOAD = 180;

  uint8_t rxBuffer[RX_CAPACITY] = {};
  uint8_t txBuffer[TX_CAPACITY] = {};
  size_t rxHead = 0;
  size_t rxTail = 0;
  size_t txHead = 0;
  size_t txTail = 0;
  InputSource inputSource = InputSource::NONE;
  portMUX_TYPE bufferMux = portMUX_INITIALIZER_UNLOCKED;

  NimBLEServer *server = nullptr;
  NimBLECharacteristic *txCharacteristic = nullptr;
  volatile bool bleReady = false;
  volatile bool bleConnected = false;
  volatile bool bleSubscribed = false;
  volatile uint16_t connectionHandle = 0xffff;
  volatile uint16_t peerMtu = 23;

  size_t rxCount();
  int readBle(bool remove);
  void enqueueBleOutput(const uint8_t *buffer, size_t size);
};

extern MarauderBleSerial MarauderSerial;
