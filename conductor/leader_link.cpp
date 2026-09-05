// NimBLE server for web/control.html. See leader_link.h for why the phone talks
// to the leader rather than to badges.
#ifdef LEADER_BLE

#include "leader_link.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "../effects/effects.h"
#include "chorus_command.h"
#include "conductor_config.h"
#include "net_espnow.h"

static NimBLEServer *s_server = nullptr;
static NimBLECharacteristic *s_state = nullptr;
static NimBLECharacteristic *s_roster = nullptr;
static volatile bool s_connected = false;

// One slot is enough. A second press before the loop has drained the first is
// a person pressing faster than 30Hz, which does not happen; a ring buffer here
// would be machinery in service of nothing.
static portMUX_TYPE s_cmdMux = portMUX_INITIALIZER_UNLOCKED;
static LeaderControlFrame s_pending;
static volatile bool s_hasPending = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
    (void)desc;
    s_connected = true;
    Serial.println("[ble] phone connected");
    // Keep advertising: a second phone taking over from a flat one should not
    // require somebody to walk to the leader and reset it.
    server->startAdvertising();
  }
  void onDisconnect(NimBLEServer *server) override {
    (void)server;
    s_connected = false;
    Serial.println("[ble] phone disconnected");
    NimBLEDevice::startAdvertising();
  }
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    const std::string v = c->getValue();
    if (v.size() < sizeof(LeaderControlFrame)) return;
    portENTER_CRITICAL(&s_cmdMux);
    memcpy(&s_pending, v.data(), sizeof(LeaderControlFrame));
    s_hasPending = true;
    portEXIT_CRITICAL(&s_cmdMux);
  }
};

// The roster is rebuilt on every read rather than cached, so a phone that pulls
// to refresh gets what the leader knows at that instant.
class RosterCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c) override {
    ChorusRadio::rosterExpire(ROSTER_STALE_MS);
    const uint32_t now = millis();
    const int n = ChorusRadio::rosterCount();
    uint8_t buf[1 + ChorusRadio::ROSTER_MAX * sizeof(LeaderRosterEntry)];
    buf[0] = (uint8_t)n;
    int out = 1;
    for (int i = 0; i < n; i++) {
      const ChorusRadio::RosterEntry *e = ChorusRadio::rosterAt(i);
      if (!e) continue;
      LeaderRosterEntry r;
      memcpy(r.id, e->id, 3);
      r.shader = e->shader;
      r.flags = e->flags;
      r.fps = e->fps;
      r.crest = e->crest;
      const uint32_t ageS = (now - e->lastSeenMs) / 1000u;
      r.ageS = (uint8_t)(ageS > 255 ? 255 : ageS);
      memcpy(buf + out, &r, sizeof(r));
      out += sizeof(r);
    }
    c->setValue(buf, out);
  }
};

bool leaderLinkTakeCommand(LeaderControlFrame *out) {
  if (!s_hasPending) return false;
  portENTER_CRITICAL(&s_cmdMux);
  *out = s_pending;
  s_hasPending = false;
  portEXIT_CRITICAL(&s_cmdMux);
  return true;
}

bool leaderLinkConnected() { return s_connected; }

bool leaderLinkBegin() {
  // The Bluetooth controller and WiFi share one radio, and enabling BT while
  // WiFi power save is off aborts inside coex_core_enable(). Measured on a
  // badge before this line existed; the leader pays the same tax. The leader
  // mostly transmits, so modem sleep costs it little -- what it does cost is
  // the occasional missed roster beacon, which is why beacons repeat every two
  // seconds and a roll call exists.
  // esp_wifi_set_ps() directly rather than WiFi.setSleep(): the Arduino wrapper
  // is a no-op that only logs when it does not like the current mode, and a
  // silently skipped call here is an abort() three lines later rather than an
  // error. ChorusRadio::bringUp() has already asked for WIFI_PS_NONE -- correct
  // for a radio that only has to broadcast music, and fatal the moment a
  // Bluetooth controller wants a share of the same antenna.
  const esp_err_t psErr = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  if (psErr != ESP_OK) {
    Serial.printf("[ble] esp_wifi_set_ps failed: %d -- not starting BLE\n", (int)psErr);
    return false;
  }
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  Serial.printf("[ble] wifi power save now %d (1 = min modem, required for coex)\n", (int)ps);
  if (ps == WIFI_PS_NONE) {
    Serial.println("[ble] power save did not take -- not starting BLE");
    return false;
  }

  NimBLEDevice::init(LEADER_LINK_NAME);
  NimBLEDevice::setMTU(247);
  s_server = NimBLEDevice::createServer();
  if (!s_server) {
    Serial.println("[ble] createServer failed");
    return false;
  }
  s_server->setCallbacks(new ServerCallbacks());

  NimBLEService *svc = s_server->createService(LEADER_LINK_SERVICE_UUID);

  NimBLECharacteristic *control =
      svc->createCharacteristic(LEADER_LINK_CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
  control->setCallbacks(new ControlCallbacks());

  s_state = svc->createCharacteristic(LEADER_LINK_STATE_UUID,
                                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  s_roster = svc->createCharacteristic(LEADER_LINK_ROSTER_UUID,
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_roster->setCallbacks(new RosterCallbacks());

  // The names are fixed for the life of a build, so they are written once here
  // rather than assembled per read. The page asks for them instead of shipping
  // its own copy of the registry: effects are append-only, and a page published
  // before an effect existed must still be able to name it.
  {
    std::string names;
    for (int i = 0; i < effects_count; i++) {
      if (i) names += "\n";
      names += effects_all[i]->name;
    }
    NimBLECharacteristic *n =
        svc->createCharacteristic(LEADER_LINK_NAMES_UUID, NIMBLE_PROPERTY::READ);
    n->setValue((const uint8_t *)names.data(), names.size());
  }

  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(LEADER_LINK_SERVICE_UUID);
  // The service UUID has to be advertised, not merely present: Web Bluetooth
  // filters the chooser on it, and a leader that only exposes the service after
  // connecting would never appear in the list to be connected to.
  adv->setScanResponse(true);
  NimBLEDevice::startAdvertising();

  Serial.printf("[ble] advertising as \"%s\", service %s\n", LEADER_LINK_NAME,
                LEADER_LINK_SERVICE_UUID);
  return true;
}

void leaderLinkPublish(uint8_t shader, uint8_t effectCount, uint8_t badges, bool hearing,
                       uint16_t cycleS, uint16_t txPerSec, uint16_t uptimeS, uint8_t bass,
                       uint8_t energy) {
  if (!s_state) return;
  LeaderStateFrame f;
  f.shader = shader;
  f.effectCount = effectCount;
  f.badges = badges;
  f.hearing = hearing ? 1 : 0;
  f.cycleS = cycleS;
  f.txPerSec = txPerSec;
  f.uptimeS = uptimeS;
  f.bass = bass;
  f.energy = energy;
  s_state->setValue((const uint8_t *)&f, sizeof(f));
  if (s_connected) s_state->notify();
}

#endif  // LEADER_BLE
