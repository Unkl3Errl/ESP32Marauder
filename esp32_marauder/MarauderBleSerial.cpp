#ifdef MARAUDER_HELTEC_V4

#include "MarauderBleSerial.h"

#include <NimBLEDevice.h>

namespace {
constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

class MarauderServerCallbacks final : public NimBLEServerCallbacks {
public:
  explicit MarauderServerCallbacks(MarauderBleSerial &owner) : owner(owner) {}

  void onConnect(NimBLEServer *, ble_gap_conn_desc *description) override {
    owner.setBleConnected(true, description->conn_handle);
  }

  void onDisconnect(NimBLEServer *server, ble_gap_conn_desc *) override {
    owner.setBleConnected(false);
    if (owner.isBleReady()) server->startAdvertising();
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *) override {
    owner.setBleMtu(mtu);
  }

private:
  MarauderBleSerial &owner;
};

class MarauderCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
public:
  explicit MarauderCharacteristicCallbacks(MarauderBleSerial &owner) : owner(owner) {}

  void onWrite(NimBLECharacteristic *characteristic, ble_gap_conn_desc *) override {
    const std::string value = characteristic->getValue();
    owner.receiveBle(reinterpret_cast<const uint8_t *>(value.data()), value.size());
  }

  void onSubscribe(NimBLECharacteristic *, ble_gap_conn_desc *, uint16_t value) override {
    owner.setBleSubscribed((value & 0x01) != 0);
  }

private:
  MarauderBleSerial &owner;
};

MarauderServerCallbacks serverCallbacks(MarauderSerial);
MarauderCharacteristicCallbacks characteristicCallbacks(MarauderSerial);
} // namespace

MarauderBleSerial MarauderSerial;

void MarauderBleSerial::begin(unsigned long baud) {
  ::Serial.begin(baud);
}

MarauderBleSerial::operator bool() const {
  return static_cast<bool>(::Serial);
}

size_t MarauderBleSerial::rxCount() {
  portENTER_CRITICAL(&bufferMux);
  const size_t count = (rxHead + RX_CAPACITY - rxTail) % RX_CAPACITY;
  portEXIT_CRITICAL(&bufferMux);
  return count;
}

int MarauderBleSerial::available() {
  if (inputSource == InputSource::USB) {
    const int count = ::Serial.available();
    if (count > 0) return count;
    inputSource = InputSource::NONE;
  } else if (inputSource == InputSource::BLE) {
    const size_t count = rxCount();
    if (count > 0) return static_cast<int>(count);
    inputSource = InputSource::NONE;
  }

  if (::Serial.available() > 0) {
    inputSource = InputSource::USB;
    return ::Serial.available();
  }

  const size_t count = rxCount();
  if (count > 0) inputSource = InputSource::BLE;
  return static_cast<int>(count);
}

int MarauderBleSerial::readBle(bool remove) {
  portENTER_CRITICAL(&bufferMux);
  if (rxTail == rxHead) {
    portEXIT_CRITICAL(&bufferMux);
    return -1;
  }
  const uint8_t value = rxBuffer[rxTail];
  if (remove) rxTail = (rxTail + 1) % RX_CAPACITY;
  portEXIT_CRITICAL(&bufferMux);
  return value;
}

int MarauderBleSerial::read() {
  if (inputSource == InputSource::NONE && available() == 0) return -1;
  const int value = inputSource == InputSource::USB ? ::Serial.read() : readBle(true);
  if (value == '\n' || value == '\r') inputSource = InputSource::NONE;
  return value;
}

int MarauderBleSerial::peek() {
  if (inputSource == InputSource::NONE && available() == 0) return -1;
  return inputSource == InputSource::USB ? ::Serial.peek() : readBle(false);
}

void MarauderBleSerial::flush() {
  ::Serial.flush();
}

size_t MarauderBleSerial::write(uint8_t value) {
  return write(&value, 1);
}

size_t MarauderBleSerial::write(const uint8_t *buffer, size_t size) {
  if (buffer == nullptr || size == 0) return 0;
  const size_t written = ::Serial.write(buffer, size);
  enqueueBleOutput(buffer, size);
  return written;
}

void MarauderBleSerial::enqueueBleOutput(const uint8_t *buffer, size_t size) {
  if (!bleReady || !bleConnected || !bleSubscribed) return;
  portENTER_CRITICAL(&bufferMux);
  for (size_t index = 0; index < size; ++index) {
    const size_t next = (txHead + 1) % TX_CAPACITY;
    if (next == txTail) break;
    txBuffer[txHead] = buffer[index];
    txHead = next;
  }
  portEXIT_CRITICAL(&bufferMux);
}

void MarauderBleSerial::beginBle() {
  if (bleReady) return;

  NimBLEDevice::init("Marauder");
  NimBLEDevice::setMTU(185);
  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);

  NimBLEService *service = server->createService(SERVICE_UUID);
  NimBLECharacteristic *rxCharacteristic = service->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 512);
  txCharacteristic = service->createCharacteristic(
      TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 512);
  rxCharacteristic->setCallbacks(&characteristicCallbacks);
  txCharacteristic->setCallbacks(&characteristicCallbacks);
  service->start();

  NimBLEAdvertising *advertising = server->getAdvertising();
  advertising->reset();
  advertising->setName("Marauder");
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  bleReady = true;
  server->startAdvertising();
}

void MarauderBleSerial::prepareForBleDeinit() {
  bleReady = false;
  bleConnected = false;
  bleSubscribed = false;
  server = nullptr;
  txCharacteristic = nullptr;
  connectionHandle = 0xffff;
  peerMtu = 23;
  inputSource = InputSource::NONE;
  portENTER_CRITICAL(&bufferMux);
  rxHead = rxTail = 0;
  txHead = txTail = 0;
  portEXIT_CRITICAL(&bufferMux);
}

void MarauderBleSerial::loop() {
  if (!bleReady || !bleConnected || !bleSubscribed || txCharacteristic == nullptr) return;

  uint8_t packet[MAX_NOTIFY_PAYLOAD];
  size_t payloadLimit = peerMtu > 3 ? peerMtu - 3 : 20;
  payloadLimit = constrain(payloadLimit, static_cast<size_t>(20), MAX_NOTIFY_PAYLOAD);

  for (uint8_t burst = 0; burst < 2; ++burst) {
    size_t count = 0;
    portENTER_CRITICAL(&bufferMux);
    while (txTail != txHead && count < payloadLimit) {
      packet[count++] = txBuffer[txTail];
      txTail = (txTail + 1) % TX_CAPACITY;
    }
    portEXIT_CRITICAL(&bufferMux);
    if (count == 0) break;
    txCharacteristic->notify(packet, count);
  }
}

void MarauderBleSerial::receiveBle(const uint8_t *data, size_t size) {
  if (data == nullptr || size == 0) return;
  portENTER_CRITICAL(&bufferMux);
  for (size_t index = 0; index < size; ++index) {
    const size_t next = (rxHead + 1) % RX_CAPACITY;
    if (next == rxTail) break;
    rxBuffer[rxHead] = data[index];
    rxHead = next;
  }
  portEXIT_CRITICAL(&bufferMux);
}

void MarauderBleSerial::setBleConnected(bool connected, uint16_t handle) {
  bleConnected = connected;
  connectionHandle = connected ? handle : 0xffff;
  if (!connected) {
    bleSubscribed = false;
    peerMtu = 23;
  }
}

void MarauderBleSerial::setBleSubscribed(bool subscribed) {
  bleSubscribed = subscribed;
}

void MarauderBleSerial::setBleMtu(uint16_t mtu) {
  peerMtu = mtu;
}

bool MarauderBleSerial::isBleReady() const {
  return bleReady;
}

#endif // MARAUDER_HELTEC_V4
