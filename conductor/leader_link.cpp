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
#include "../effects/knobs.h"
#include "chorus_command.h"
#include "conductor_config.h"
#include "net_espnow.h"

static NimBLEServer *s_server = nullptr;
static NimBLECharacteristic *s_state = nullptr;
static NimBLECharacteristic *s_roster = nullptr;
static NimBLECharacteristic *s_labels = nullptr;
static NimBLECharacteristic *s_effectChar = nullptr;
static NimBLECharacteristic *s_knobChar[KNOB_COUNT] = {nullptr};
static NimBLEDescriptor *s_knobDesc[KNOB_COUNT] = {nullptr};
static volatile bool s_connected = false;

// A short ring, not one slot. Buttons arrive slower than the loop drains, but
// knobs do not: a slider drag is a stream of writes, and a page restoring a
// saved look writes eight knobs in a row. One slot would keep only the last of
// them and silently lose the rest.
static portMUX_TYPE s_cmdMux = portMUX_INITIALIZER_UNLOCKED;
static constexpr int CMD_RING = 16;
static LeaderControlFrame s_ring[CMD_RING];
static volatile uint8_t s_ringHead = 0, s_ringTail = 0;

static void queueFrame(const LeaderControlFrame &f) {
  portENTER_CRITICAL(&s_cmdMux);
  const uint8_t next = (uint8_t)((s_ringHead + 1) % CMD_RING);
  if (next != s_ringTail) {
    s_ring[s_ringHead] = f;
    s_ringHead = next;
  }
  portEXIT_CRITICAL(&s_cmdMux);
}

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

// A one-byte write turns into the same LeaderControlFrame the binary
// characteristic produces, so poking a byte in nRF Connect and tapping the web
// page take literally the same path through the firmware.
class ByteCallbacks : public NimBLECharacteristicCallbacks {
 public:
  ByteCallbacks(uint8_t op, uint8_t arg0) : _op(op), _arg0(arg0) {}
  void onWrite(NimBLECharacteristic *c) override {
    const std::string v = c->getValue();
    if (v.empty()) return;
    LeaderControlFrame f = {};
    f.op = _op;
    // Knobs carry the knob number in arg0 and the value in arg1; effect and
    // crest carry their value in arg0 and ignore _arg0.
    if (_op == LEADER_OP_SET_KNOB) {
      f.arg0 = _arg0;
      f.arg1 = (uint8_t)v[0];
    } else {
      f.arg0 = (uint8_t)v[0];
    }
    queueFrame(f);
  }

 private:
  uint8_t _op, _arg0;
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    const std::string v = c->getValue();
    if (v.size() < sizeof(LeaderControlFrame)) return;
    LeaderControlFrame f;
    memcpy(&f, v.data(), sizeof(f));
    queueFrame(f);
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
  bool got = false;
  portENTER_CRITICAL(&s_cmdMux);
  if (s_ringTail != s_ringHead) {
    *out = s_ring[s_ringTail];
    s_ringTail = (uint8_t)((s_ringTail + 1) % CMD_RING);
    got = true;
  }
  portEXIT_CRITICAL(&s_cmdMux);
  return got;
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

  // --- the pokeable bytes --------------------------------------------------
  // Each of these is one byte with a 0x2901 user description, which is what a
  // generic BLE scanner renders as a label next to an editable value. It is
  // the difference between "c8a0f210" holding 0x50 and "knob 1 (reactivity)"
  // holding 80.
  //
  // setValue() must be given an explicit pointer and length. NimBLE's
  // setValue(const T&) template happily accepts a `const char *` and memcpy's
  // the four bytes of the POINTER into the descriptor, which a scanner then
  // renders as garbage -- observed exactly that way before this comment
  // existed, four bytes of "\xb0\x1a\x3f?" where "knob 1" should have been.
  auto describe = [](NimBLECharacteristic *c, const char *text) {
    NimBLEDescriptor *d = c->createDescriptor("2901", NIMBLE_PROPERTY::READ, 40);
    if (d) d->setValue((const uint8_t *)text, strlen(text));
  };

  s_effectChar = svc->createCharacteristic(
      LEADER_LINK_EFFECT_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  s_effectChar->setCallbacks(new ByteCallbacks(LEADER_OP_SET_EFFECT, 0));
  describe(s_effectChar, "effect (index)");

  NimBLECharacteristic *crest =
      svc->createCharacteristic(LEADER_LINK_CREST_UUID, NIMBLE_PROPERTY::WRITE);
  crest->setCallbacks(new ByteCallbacks(LEADER_OP_SET_CREST, 0));
  describe(crest, "crest (index)");

  // The knob descriptions carry the CURRENT effect's label, because that is
  // what makes them usable in a scanner. They are refreshed on every effect
  // change -- a scanner already connected will not re-read them, which is why
  // the labels characteristic exists as well and why the slot meanings in
  // knobs.h are kept consistent across effects.
  for (int i = 0; i < KNOB_COUNT; i++) {
    char uuid[48];
    snprintf(uuid, sizeof(uuid), LEADER_LINK_KNOB_BASE_UUID, i);
    s_knobChar[i] = svc->createCharacteristic(
        uuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    s_knobChar[i]->setCallbacks(new ByteCallbacks(LEADER_OP_SET_KNOB, (uint8_t)i));
    char label[40];
    snprintf(label, sizeof(label), "knob %d", i + 1);
    describe(s_knobChar[i], label);
    s_knobDesc[i] = s_knobChar[i]->getDescriptorByUUID("2901");
  }

  s_labels = svc->createCharacteristic(LEADER_LINK_LABELS_UUID,
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

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

// Called whenever the effect changes, and once at start-up: refreshes the knob
// values a scanner reads, and the label block the page reads.
void leaderLinkPublishKnobs(uint8_t shader) {
  const KnobSpec *ks = effect_knob_specs(shader);
  for (int i = 0; i < KNOB_COUNT; i++) {
    if (!s_knobChar[i]) continue;
    // "knob 3 (spin)" rather than "knob 3", so a scanner shows what the slot
    // currently means. An app already connected will not re-read this, which
    // is why knobs.h keeps slot meanings consistent between effects anyway.
    if (s_knobDesc[i]) {
      char label[40];
      if (ks[i].name) snprintf(label, sizeof(label), "knob %d (%s)", i + 1, ks[i].name);
      else snprintf(label, sizeof(label), "knob %d (unused here)", i + 1);
      s_knobDesc[i]->setValue((const uint8_t *)label, strlen(label));
    }
    const uint8_t v = knob_raw(i);
    s_knobChar[i]->setValue(&v, 1);
    if (s_connected) s_knobChar[i]->notify();
  }
  if (s_effectChar) {
    s_effectChar->setValue(&shader, 1);
    if (s_connected) s_effectChar->notify();
  }
  if (s_labels) {
    // KNOB_COUNT lines of knob label (blank where unused), then "--", then one
    // line per crest. One read gives a UI everything it needs to label itself.
    std::string out;
    const KnobSpec *spec = effect_knob_specs(shader);
    for (int i = 0; i < KNOB_COUNT; i++) {
      out += spec[i].name ? spec[i].name : "";
      out += "\n";
    }
    out += "--\n";
    for (int i = 0; i < mon_variant_count(); i++) {
      out += mon_variant_name(i);
      if (i + 1 < mon_variant_count()) out += "\n";
    }
    s_labels->setValue((const uint8_t *)out.data(), out.size());
    if (s_connected) s_labels->notify();
  }
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
