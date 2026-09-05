// BLE side of the phone-to-badge link. See ble_control.h for the threading
// rule: callbacks only park data; loop() applies it.
#include "ble_control.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <string.h>

static NimBLECharacteristic *s_ctrl = nullptr;
static NimBLECharacteristic *s_status = nullptr;
static NimBLECharacteristic *s_catalog = nullptr;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static PhoneControlFrame s_current = {PHONE_CONTROL_FOLLOW, PHONE_CONTROL_DEFAULT, 255, 0};
static PhoneControlFrame s_pending;
static volatile bool s_hasPending = false;
static volatile bool s_connected = false;

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    NimBLEAttValue v = c->getValue();
    const size_t n = v.length();
    if (n == 0) return;
    const uint8_t *d = v.data();
    portENTER_CRITICAL(&s_mux);
    PhoneControlFrame f = s_hasPending ? s_pending : s_current;
    f.effect = d[0];
    if (n >= 2) f.crest = d[1];
    if (n >= 3) f.brightness = d[2];
    if (n >= 4) f.flags = d[3];
    s_pending = f;
    s_hasPending = true;
    portEXIT_CRITICAL(&s_mux);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override {
    s_connected = true;
    Serial.println("[ble] phone connected");
  }
  void onDisconnect(NimBLEServer *) override {
    s_connected = false;
    Serial.println("[ble] phone disconnected, advertising again");
    NimBLEDevice::startAdvertising();
  }
};

void bleControlInit(const char *name, const char *catalog, const PhoneControlFrame &initial) {
  s_current = initial;
  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(128);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *svc = server->createService(PHONE_LINK_SERVICE_UUID);

  s_ctrl = svc->createCharacteristic(
      PHONE_CONTROL_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY,
      sizeof(PhoneControlFrame));
  s_ctrl->setValue((const uint8_t *)&s_current, sizeof(s_current));
  s_ctrl->setCallbacks(new ControlCallbacks());

  s_catalog = svc->createCharacteristic(PHONE_CATALOG_UUID, NIMBLE_PROPERTY::READ, 512);
  s_catalog->setValue((const uint8_t *)catalog, strlen(catalog));

  s_status = svc->createCharacteristic(PHONE_LINK_STATUS_UUID,
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                       sizeof(PhoneStatusFrame));
  PhoneStatusFrame zero = {};
  s_status->setValue((const uint8_t *)&zero, sizeof(zero));

  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(PHONE_LINK_SERVICE_UUID);
  adv->setScanResponse(true);  // 128-bit UUID plus the name do not fit one packet
  adv->start();
  Serial.printf("[ble] advertising as %s\n", name);
}

bool bleControlPoll(PhoneControlFrame *out) {
  if (!s_hasPending) return false;
  portENTER_CRITICAL(&s_mux);
  *out = s_pending;
  s_hasPending = false;
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void bleControlPublish(const PhoneControlFrame &now) {
  portENTER_CRITICAL(&s_mux);
  s_current = now;
  portEXIT_CRITICAL(&s_mux);
  if (!s_ctrl) return;
  s_ctrl->setValue((const uint8_t *)&now, sizeof(now));
  if (s_connected) s_ctrl->notify();
}

void bleControlStatus(const PhoneStatusFrame &st) {
  if (!s_status) return;
  s_status->setValue((const uint8_t *)&st, sizeof(st));
  if (s_connected) s_status->notify();
}

bool bleControlConnected() { return s_connected; }
