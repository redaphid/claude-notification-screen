// BLE side of the phone-as-conductor path.
//
// The badge advertises a tiny GATT service. A phone connects, writes 8-byte
// feature frames at ~30Hz, and this turns them into the same ChorusPacket the
// rest of the swarm already speaks. Nothing else in the swarm needs to know a
// phone is involved.
#include "phone_link.h"

// Compiles to nothing without the flag. A giveaway badge should not carry a BLE
// stack it never uses, and the plain env must not need NimBLE installed at all.
#ifdef BADGE_PHONE_LINK

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include "chorus_packet.h"

namespace {

// Written from the NimBLE stack task, read from the render loop.
portMUX_TYPE phoneMux = portMUX_INITIALIZER_UNLOCKED;
PhoneFeatureFrame lastFrame = {};
volatile uint32_t lastFrameMs = 0;
volatile uint32_t framesAccepted = 0;
volatile bool phoneConnected = false;

NimBLECharacteristic *statusChar = nullptr;

class FeatureSink : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    const std::string v = c->getValue();
    if (v.size() != sizeof(PhoneFeatureFrame)) return;  // wrong shape, ignore
    PhoneFeatureFrame f;
    memcpy(&f, v.data(), sizeof(f));
    portENTER_CRITICAL(&phoneMux);
    lastFrame = f;
    lastFrameMs = millis();
    framesAccepted++;
    portEXIT_CRITICAL(&phoneMux);
  }
};

class LinkState : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override {
    phoneConnected = true;
    Serial.println("[phone] connected");
  }
  void onDisconnect(NimBLEServer *s) override {
    phoneConnected = false;
    Serial.println("[phone] disconnected");
    // Keep advertising: a badge that stops being findable after one phone
    // walks away is a badge nobody can use again without a power cycle.
    s->startAdvertising();
  }
};

FeatureSink featureSink;
LinkState linkState;

}  // namespace

bool phoneLinkBegin() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[24];
  snprintf(name, sizeof(name), PHONE_LINK_NAME_PREFIX "%02X%02X", mac[4], mac[5]);

  NimBLEDevice::init(name);
  // The radio is shared with ESP-NOW. Transmitting BLE at full power buys
  // nothing here -- a phone is in the same pocket or hand as the badge -- and
  // costs airtime the swarm needs.
  NimBLEDevice::setPower(ESP_PWR_LVL_N12);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&linkState);

  NimBLEService *service = server->createService(PHONE_LINK_SERVICE_UUID);

  NimBLECharacteristic *features = service->createCharacteristic(
      PHONE_LINK_FEATURES_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  features->setCallbacks(&featureSink);

  statusChar = service->createCharacteristic(PHONE_LINK_STATUS_UUID, NIMBLE_PROPERTY::NOTIFY |
                                                                        NIMBLE_PROPERTY::READ);

  service->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(PHONE_LINK_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  Serial.printf("[phone] BLE up as \"%s\", advertising %s\n", name, PHONE_LINK_SERVICE_UUID);
  return true;
}

bool phoneLinkFresh(uint32_t now) {
  portENTER_CRITICAL(&phoneMux);
  const uint32_t last = lastFrameMs;
  const uint32_t frames = framesAccepted;
  portEXIT_CRITICAL(&phoneMux);
  return frames > 0 && (now - last) <= PHONE_LINK_STALE_MS;
}

bool phoneLinkRead(float features[FEAT_COUNT], uint8_t *beat, uint8_t *shader) {
  PhoneFeatureFrame f;
  portENTER_CRITICAL(&phoneMux);
  f = lastFrame;
  portEXIT_CRITICAL(&phoneMux);

  features[FEAT_BASS] = f.bass / 255.0f;
  features[FEAT_MID] = f.mid / 255.0f;
  features[FEAT_TREBLE] = f.treble / 255.0f;
  features[FEAT_ENERGY] = f.energy / 255.0f;
  *beat = f.beat;
  *shader = f.shader;
  return true;
}

void phoneLinkPublishStatus(uint8_t role, bool espnowHeard, uint16_t rxRate, uint16_t txRate) {
  if (!statusChar || !phoneConnected) return;
  PhoneStatusFrame s;
  s.role = role;
  s.espnowHeard = espnowHeard ? 1 : 0;
  s.rxRate = rxRate;
  s.txRate = txRate;
  portENTER_CRITICAL(&phoneMux);
  s.frames = (uint16_t)framesAccepted;
  portEXIT_CRITICAL(&phoneMux);
  statusChar->setValue((uint8_t *)&s, sizeof(s));
  statusChar->notify();
}

bool phoneLinkConnected() { return phoneConnected; }

#endif  // BADGE_PHONE_LINK
