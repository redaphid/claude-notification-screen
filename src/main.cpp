// Badge firmware, Stage 1: receive ChorusPacket over ESP-NOW, relay it to the
// rest of the swarm, render a visual locally.
//
// Roles are chosen at boot, not compiled in: hold the BOOT button while the
// board resets and this badge becomes the conductor (running a mock DJ until
// the real microphone conductor exists). Release it and the badge is deaf --
// it listens, relays, and renders. Same binary on every board in the bag.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_system.h>
#include <math.h>

#include "chorus_command.h"
#include "chorus_packet.h"
#include "display.h"
#include "effects.h"
#include "phone_link.h"

static RoundBadgeDisplay display;
static LGFX_Sprite canvas(&display);

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// Broadcast cadence. This is checked once per rendered frame, so it must not
// sit just above the frame time or it aliases: at 31fps (32ms/frame) a 33ms
// interval is only satisfied every second frame, which silently halves the
// packet rate to ~16Hz. Measured on hardware before this was 30. Keep it below
// the frame time and let the render loop set the real cadence.
static constexpr uint32_t CONDUCTOR_INTERVAL_MS = 30;
static constexpr uint32_t FEATURE_STALE_MS = 300;
static constexpr uint16_t MOCK_BPM = 118;

static bool isConductor = false;
static bool radioUp = false;
static bool phoneLinkUp = false;

// Who is conducting this badge right now.
//
// The rule is deliberately simple and always favours the swarm over the phone:
// a real conductor on the air wins, every time. A phone only takes over when
// nothing has been heard for PHONE_LINK_YIELD_MS. Otherwise two sources drive
// one swarm and the badges tear between them -- and because sequence numbers
// come from different counters, each source would look to the other like a
// conductor that had just restarted, so they would fight rather than blend.
//
// The consequence worth knowing: if two phones connect to two different badges
// in a silent room, both become conductors and the swarm will split. The packet
// carries no source identity to arbitrate with, and it is frozen, so this is
// documented rather than solved. See docs/adr-002.
enum ConductorSource : uint8_t { SRC_NONE = 0, SRC_PHONE = 1, SRC_ESPNOW = 2 };
static ConductorSource activeSource = SRC_NONE;
// Set when the phone reports an onset, consumed by the next rendered frame.
static bool phoneOnset = false;

// Counts boots that never reached steady rendering, so a badge can notice it is
// caught in a boot loop: if the radio brings the rail down three times running
// -- a tired power bank, a thin cable -- the next boot skips the radio and the
// badge renders locally. A giveaway badge that looks dead is worse than one
// that is merely alone.
//
// This lives in flash, NOT in RTC memory. RTC memory survives a panic or a
// software reset, but a brownout is a power loss and wipes it -- which is
// exactly the failure this counter exists to escape. Measured on hardware: the
// RTC version never counted past 1 while the board looped.
static Preferences prefs;
static uint32_t bootAttempts = 0;
static constexpr uint32_t BOOT_ATTEMPTS_BEFORE_GIVING_UP_ON_RADIO = 3;

// --- feature state -------------------------------------------------------
// Written from the ESP-NOW receive callback (WiFi task), read from the render
// loop, so it lives behind a spinlock rather than being merely volatile.
static portMUX_TYPE featureMux = portMUX_INITIALIZER_UNLOCKED;
static float rxFeatures[FEAT_COUNT] = {0, 0, 0, 0};
static uint32_t rxLastMs = 0;
static uint16_t rxLastSeq = 0;
static bool rxSeen = false;
static uint32_t rxCount = 0;
static uint32_t relayCount = 0;
static uint8_t rxShader = 0;

// Whether a packet actually left the antenna, rather than merely being handed
// to the driver. A conductor that thinks it is broadcasting into a silent
// swarm is the hardest failure to diagnose in a field.
static uint32_t txOk = 0, txFail = 0;

// How far out of order a packet may be and still count as a straggler rather
// than evidence that the conductor restarted. At 30Hz this is ~8s of history.
static constexpr int16_t SEQ_REORDER_WINDOW = 256;
static uint32_t resyncCount = 0;

// Who did this badge actually hear, and how far had the packet travelled?
//
// With one conductor and one badge, `rx` was unambiguous. With several benches
// in radio range of each other it is not: a packet may come straight from the
// conductor or via somebody else's badge relaying it, and the counters cannot
// tell those apart. The ESP-NOW callback hands us the MAC of the immediate
// transmitter, which is exactly the distinction needed -- origin versus relay.
//
// This is what makes a mesh test readable: hop 0 is heard directly, hop 1 came
// through one other node, and a badge that only ever sees hop 2 is being
// carried by the swarm rather than by the conductor.
static constexpr int MAX_SOURCES = 8;
struct SourceStat {
  uint8_t mac[6];
  uint32_t count;
  uint8_t lastHop;
  uint32_t lastMs;
  bool used;
};
static SourceStat sources[MAX_SOURCES];
static uint32_t rxByHop[CHORUS_MAX_HOP + 2];

// Called from the WiFi task, already inside the feature spinlock.
static void noteSource(const uint8_t *mac, uint8_t hop) {
  if (hop < (uint8_t)(CHORUS_MAX_HOP + 2)) rxByHop[hop]++;
  for (int i = 0; i < MAX_SOURCES; i++) {
    if (sources[i].used && memcmp(sources[i].mac, mac, 6) == 0) {
      sources[i].count++;
      sources[i].lastHop = hop;
      sources[i].lastMs = millis();
      return;
    }
  }
  for (int i = 0; i < MAX_SOURCES; i++) {
    if (!sources[i].used) {
      memcpy(sources[i].mac, mac, 6);
      sources[i].used = true;
      sources[i].count = 1;
      sources[i].lastHop = hop;
      sources[i].lastMs = millis();
      return;
    }
  }
  // More than MAX_SOURCES neighbours: the table is a diagnostic, not a router.
}

// Smoothed values actually handed to the visual. Asymmetric on purpose: fast
// attack so a kick lands now, slow release so a dropped packet reads as a slow
// exhale instead of a flicker.
// Relays are queued here rather than sent from the receive callback:
// esp_now_send() from inside the callback runs in the WiFi task and can
// deadlock or silently drop. A short ring is plenty -- at 30Hz a badge that
// falls more than a few packets behind should drop them, not buffer them.
static constexpr int RELAY_QUEUE_LEN = 4;
static ChorusPacket relayQueue[RELAY_QUEUE_LEN];
static volatile uint8_t relayHead = 0, relayTail = 0;
static uint32_t relayDropped = 0;

// --- remote control ------------------------------------------------------
// Commands and roster beacons are 16 bytes and rare, so they ride a second,
// shorter ring rather than widening the one the music uses. Same reason as the
// music ring: esp_now_send() must not be called from the receive callback.
struct AuxFrame {
  uint8_t len;
  uint8_t bytes[sizeof(ChorusCommand) > sizeof(ChorusHello) ? sizeof(ChorusCommand)
                                                            : sizeof(ChorusHello)];
};
static constexpr int AUX_QUEUE_LEN = 6;
static AuxFrame auxQueue[AUX_QUEUE_LEN];
static volatile uint8_t auxHead = 0, auxTail = 0;
static uint32_t auxDropped = 0;

// This badge's name on the air: the last three bytes of its STA MAC.
static uint8_t myId[3] = {0, 0, 0};
// Which crest this badge wears, remembered so the roster beacon can say so.
static int myCrest = 0;

// A pin from a phone. Outranks the conductor's shader byte and outranks
// BADGE_LOCK_EFFECT -- see the note on CMD_SET_EFFECT in chorus_command.h.
static volatile bool pinned = false;
static volatile uint8_t pinnedShader = 0;
static volatile uint32_t pinUntilMs = 0;  // 0 == until released
static volatile uint32_t identifyUntilMs = 0;
static volatile uint8_t requestedBrightness = 255;
static uint8_t appliedBrightness = 255;
// A roll call answered by thirty badges in the same millisecond is thirty
// collisions. Each badge waits a slice of a second derived from its own MAC.
static volatile uint32_t helloDueMs = 0;
static uint16_t cmdLastSeq = 0;
static uint32_t cmdLastMs = 0;
static bool cmdSeen = false;
// Quiet for this long and the next command is a new epoch whatever its
// sequence says. Comfortably longer than the burst of repeats one command
// sends, and far shorter than the gap between two presses of a button.
static constexpr uint32_t CMD_EPOCH_MS = 3000;
static uint32_t cmdCount = 0;
static uint8_t helloSeq = 0;
static uint32_t helloSent = 0;
static uint32_t helloFps = 0;
static uint32_t helloRxPerSec = 0;

// Dedupe for relayed beacons: one slot per badge we have heard from, holding
// the last hello sequence seen. Without it a hello ping-pongs between two
// badges that can both hear each other.
static constexpr int HELLO_SEEN_MAX = 24;
struct HelloSeen {
  uint8_t id[3];
  uint8_t seq;
  bool used;
};
static HelloSeen helloSeen[HELLO_SEEN_MAX];

// Values handed to the visual. Deliberately NOT smoothed: the conductor already
// ships designed attack-decay envelopes, and filtering them again here would
// reintroduce exactly the lag that shaping them was meant to remove. The only
// time-based term is `presence`, which fades a badge out when it stops hearing
// a conductor -- a designed release, not a filter.
static float shown[FEAT_COUNT] = {0, 0, 0, 0};
static float presence = 0.0f;

// --- visuals ----------------------------------------------------------
// Effects live in effects/, compile unchanged for the desktop harness, and are
// indexed by the packet's shader byte so "everyone switch to 3" needs no table
// here. The temporary inline plasma this replaced was only ever a proof that
// the render path worked.
static uint8_t activeShader = 0;

// What a badge shows when nobody is conducting: at boot, and again once the
// conductor has been silent for DEFAULT_REVERT_MS. Named, not indexed, so it
// survives the registry growing. The bag ships on the ChromaDepth crest.
#ifndef BADGE_DEFAULT_EFFECT_NAME
#define BADGE_DEFAULT_EFFECT_NAME "chroma"
#endif
static constexpr uint32_t DEFAULT_REVERT_MS = 10000;
static uint8_t defaultShader = 0;

static uint8_t resolveDefaultShader() {
  for (int i = 0; i < effects_count; i++) {
    if (strcmp(effects_all[i]->name, BADGE_DEFAULT_EFFECT_NAME) == 0) return (uint8_t)i;
  }
  return 0;
}

static void effectsInit() {
  for (int i = 0; i < effects_count; i++) {
    if (effects_all[i]->init) effects_all[i]->init();
  }
  Serial.printf("[badge] %d effects registered\n", effects_count);
}

// Has a real conductor been on the air recently? A phone must yield to one.
static bool heardRecently(uint32_t now) {
  portENTER_CRITICAL(&featureMux);
  const bool seen = rxSeen;
  const uint32_t last = rxLastMs;
  portEXIT_CRITICAL(&featureMux);
  return seen && (now - last) <= PHONE_LINK_YIELD_MS;
}

// When a badge is the conductor (BOOT held at reset), its serial console picks
// what the swarm shows, the same way the leader's does:
//   shader <n> | s <n> | <n> | plasma | tunnel | iris | mon | next | prev | ?
static uint8_t conductorShader = 0;

static void setConductorShader(int index) {
  if (effects_count <= 0) return;
  index = ((index % effects_count) + effects_count) % effects_count;
  conductorShader = (uint8_t)index;
  Serial.printf("[badge] shader -> %u (%s)\n", (unsigned)conductorShader, effects_all[conductorShader]->name);
}

static void handleSerialLine(String line) {
  line.trim();
  line.toLowerCase();
  if (line.isEmpty()) return;
  if (!isConductor) {
    Serial.println("[badge] receiver: the conductor picks the shader (hold BOOT at reset to lead)");
    return;
  }
  if (line == "?" || line == "help") {
    Serial.printf("[badge] shader %u of %d:", (unsigned)conductorShader, effects_count);
    for (int i = 0; i < effects_count; i++) Serial.printf(" %d=%s", i, effects_all[i]->name);
    Serial.println();
    return;
  }
  if (line == "next" || line == "n") { setConductorShader(conductorShader + 1); return; }
  if (line == "prev" || line == "p") { setConductorShader(conductorShader - 1); return; }
  String arg = line;
  if (line.startsWith("shader")) arg = line.substring(6);
  else if (line.startsWith("s ")) arg = line.substring(2);
  arg.trim();
  for (int i = 0; i < effects_count; i++) {
    if (arg == effects_all[i]->name) { setConductorShader(i); return; }
  }
  if (!arg.isEmpty() && isDigit(arg[0])) { setConductorShader(arg.toInt()); return; }
  Serial.printf("[badge] unknown command '%s' (try ?)\n", line.c_str());
}

static void pollSerial() {
  static String pending;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!pending.isEmpty()) handleSerialLine(pending);
      pending = "";
    } else if (pending.length() < 64) {
      pending += c;
    }
  }
}

// Which family crest this badge wears in the "mon" effect. Keyed on the last
// three MAC bytes so the mapping survives reflashes and the same badge is
// always the same bead; a badge not in the table hashes into the set, which
// still gives a stable answer per badge.
static void monSelectForThisBadge() {
  struct KnownBadge { uint32_t macTail; const char *crest; };
  static const KnownBadge known[] = {
      {0x6F29D0, "kiku"},     // COM4 on the Windows bench
      {0x6F2AC8, "tomoe"},    // COM5
      {0x6EFD7C, "kikyo"},    // COM6
      {0x6F2ACC, "ume"},      // COM7
      {0x85DCF8, "hakkaku"},  // COM8
      {0x85DC30, "mokko"},    // the Linux bench's badge
  };
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  const uint32_t tail = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  int variant = -1;
  for (const KnownBadge &k : known) {
    if (k.macTail != tail) continue;
    for (int i = 0; i < mon_variant_count(); i++) {
      if (strcmp(mon_variant_name(i), k.crest) == 0) variant = i;
    }
  }
  if (variant < 0) variant = (int)((tail * 2654435761u) >> 8) % mon_variant_count();
  mon_select(variant);
  myCrest = variant;
  Serial.printf("[badge] crest: %s (mon variant %d)\n", mon_variant_name(variant), variant);
}

// --- ESP-NOW -------------------------------------------------------------
static void broadcast(const ChorusPacket &pkt) {
  // esp_now_send() reads driver state that only exists after a successful
  // init, so calling it without one is a null dereference, not a no-op. A
  // conductor whose radio failed must keep rendering, not panic.
  if (!radioUp) return;
  esp_now_send(BROADCAST_ADDR, (const uint8_t *)&pkt, sizeof(pkt));
}

static void broadcastRaw(const void *frame, size_t len) {
  if (!radioUp) return;
  esp_now_send(BROADCAST_ADDR, (const uint8_t *)frame, len);
}

// Queue a 16-byte frame for retransmission from loop context.
static void auxEnqueue(const void *frame, uint8_t len) {
  const uint8_t next = (uint8_t)((auxHead + 1) % AUX_QUEUE_LEN);
  if (next == auxTail) {
    auxDropped++;
    return;
  }
  auxQueue[auxHead].len = len;
  memcpy(auxQueue[auxHead].bytes, frame, len);
  auxHead = next;
}

// Has this beacon already been through here? Returns true the first time only.
static bool helloIsNew(const ChorusHello &h) {
  for (int i = 0; i < HELLO_SEEN_MAX; i++) {
    if (!helloSeen[i].used || !chorusIdEq(helloSeen[i].id, h.id)) continue;
    // A badge that reboots restarts its hello counter, so an exactly-equal
    // sequence is the only thing treated as a duplicate. Anything else is news.
    if (helloSeen[i].seq == h.seq) return false;
    helloSeen[i].seq = h.seq;
    return true;
  }
  for (int i = 0; i < HELLO_SEEN_MAX; i++) {
    if (helloSeen[i].used) continue;
    memcpy(helloSeen[i].id, h.id, 3);
    helloSeen[i].seq = h.seq;
    helloSeen[i].used = true;
    return true;
  }
  return true;  // table full: relay it rather than silence a badge
}

// Apply a command aimed at this badge. Called from the WiFi task, so it only
// sets flags that loop() reads -- nothing here touches the panel or the radio.
static void applyCommand(const ChorusCommand &c) {
  const uint32_t now = millis();
  switch (c.op) {
    case CMD_SET_EFFECT:
      if (c.arg0 < (uint8_t)effects_count) {
        pinnedShader = c.arg0;
        pinUntilMs = c.arg1 ? now + (uint32_t)c.arg1 * 1000u : 0;
        pinned = true;
      }
      break;
    case CMD_RELEASE:
      pinned = false;
      pinUntilMs = 0;
      break;
    case CMD_IDENTIFY:
      identifyUntilMs = now + (uint32_t)(c.arg0 ? c.arg0 : 3) * 1000u;
      break;
    case CMD_BRIGHTNESS:
      requestedBrightness = c.arg0;
      break;
    case CMD_ROLL_CALL:
      // Spread the answers across a second so the replies do not collide.
      helloDueMs = now + (uint32_t)(chorusIdToTail(myId) % 900u);
      break;
    default:
      break;
  }
}

static void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    txOk++;
  } else {
    txFail++;
  }
}

static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  // Commands and beacons share the air with the music but not its frame: they
  // are 16 bytes with their own magic, so an old badge drops them and a new one
  // handles them here, before the feature path gets a look.
  if (chorusCommandValid(data, len)) {
    ChorusCommand c;
    memcpy(&c, data, sizeof(c));
    // The same epoch trap the feature path fell into, and it bit here too:
    // the leader's command counter restarts at zero every time it reboots, so
    // a plain "is this newer?" makes a freshly reflashed leader unable to
    // command a badge that has been up all along. Observed on this bench --
    // `pin 85dcdc iris` was accepted by the leader and silently ignored by the
    // badge, which had last seen sequence 2.
    //
    // Commands are rare, so time is the better discriminator than sequence:
    // anything after a few quiet seconds is new by definition, while the three
    // copies of one command arrive within milliseconds of each other and are
    // still deduped on sequence.
    const uint32_t cmdNowMs = millis();
    const int16_t d = (int16_t)(c.seq - cmdLastSeq);
    const bool cmdGap = (cmdNowMs - cmdLastMs) > CMD_EPOCH_MS;
    const bool fresh = !cmdSeen || d > 0 || cmdGap || d < -SEQ_REORDER_WINDOW;
    if (!fresh) return;  // a repeat, or already seen down another relay path
    cmdLastSeq = c.seq;
    cmdLastMs = cmdNowMs;
    cmdSeen = true;
    cmdCount++;
    if (chorusIdIsBroadcast(c.target) || chorusIdEq(c.target, myId)) applyCommand(c);
    // Relay regardless of who it was for: the badge it was aimed at may only be
    // reachable through this one.
    if (c.hop < CHORUS_CMD_MAX_HOP) {
      c.hop++;
      auxEnqueue(&c, sizeof(c));
    }
    return;
  }
  if (chorusHelloValid(data, len)) {
    ChorusHello h;
    memcpy(&h, data, sizeof(h));
    if (chorusIdEq(h.id, myId)) return;  // our own beacon, come back around
    if (!helloIsNew(h)) return;
    if (h.hop < CHORUS_HELLO_MAX_HOP) {
      h.hop++;
      auxEnqueue(&h, sizeof(h));
    }
    return;
  }
  if (!chorusPacketValid(data, len)) return;
  ChorusPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  // Dedupe, with an escape hatch for a conductor that restarted.
  //
  // Sequence numbers begin again at zero every time the conductor reboots --
  // a battery swap, a reset, a crash, a reflash. A plain "is this newer?"
  // test then rejects every packet until the counter climbs back past where
  // it left off, which at 30Hz is over a minute of dead swarm for a conductor
  // that had been running two minutes, and up to half an hour in the worst
  // case. Measured on two boards: the badge froze at rx 2434 and stayed dark
  // while the conductor happily transmitted.
  //
  // So a packet is also accepted when we have heard nothing recently (the
  // conductor went away and came back) or when its sequence is far enough
  // behind to be a new epoch rather than a reordered straggler. The narrow
  // reorder window is what still suppresses relay storms.
  const uint32_t nowMs = millis();
  portENTER_CRITICAL(&featureMux);
  const int16_t seqDelta = (int16_t)(pkt.seq - rxLastSeq);
  const bool silenceGap = (nowMs - rxLastMs) > FEATURE_STALE_MS;
  const bool newEpoch = seqDelta < -SEQ_REORDER_WINDOW;
  const bool fresh = !rxSeen || seqDelta > 0 || silenceGap || newEpoch;
  if (fresh && (silenceGap || newEpoch)) resyncCount++;
  if (fresh) {
    rxLastSeq = pkt.seq;
    rxSeen = true;
    rxLastMs = nowMs;
    rxCount++;
    for (int i = 0; i < FEAT_COUNT; i++) rxFeatures[i] = pkt.features[i];
    rxShader = pkt.shader;
    noteSource(mac, pkt.hop);
  }
  portEXIT_CRITICAL(&featureMux);
  if (!fresh) return;  // already relayed this one down another path

  // Mesh rebroadcast: a dense crowd of badges becomes a *good* topology.
  if (pkt.hop < CHORUS_MAX_HOP) {
    pkt.hop++;
    const uint8_t next = (uint8_t)((relayHead + 1) % RELAY_QUEUE_LEN);
    if (next == relayTail) {
      relayDropped++;  // queue full: drop the oldest news, not the newest
    } else {
      relayQueue[relayHead] = pkt;
      relayHead = next;
    }
  }
}

// Bringing up the radio is the biggest current spike this board ever draws.
// On a marginal supply -- a long USB cable, a laptop port that has dropped into
// a low-power state, a nearly-flat LiPo -- that spike browns out the 3V3 rail
// and takes the whole board (USB bridge included) down with it. Badges live on
// power banks in a field, so the radio comes up defensively: CPU dropped to
// 80MHz across the spike, transmit power turned down as soon as it is legal to
// do so, full speed restored only once the radio is running.
static bool espNowInit() {
  const int cpuBefore = getCpuFrequencyMhz();
  setCpuFrequencyMhz(80);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#ifdef BADGE_PHONE_LINK
  // BLE and ESP-NOW share one radio, and WiFi/BT coexistence can only work if
  // WiFi yields airtime -- with modem sleep disabled, esp_bt_controller_enable()
  // aborts inside coex_core_enable(). Verified by backtrace, not guessed.
  //
  // So a badge that can be conducted by a phone must let WiFi sleep, and pays
  // for it in ESP-NOW reception. That is the real cost of the phone path, and
  // it is why this is a separate build rather than the default.
  WiFi.setSleep(true);
#else
  WiFi.setSleep(false);  // ESP-NOW receive must not miss packets to modem sleep
#endif
  // Transmit power is a power-budget knob, not just a range knob: sustained
  // transmit is what collapses a marginal supply. Lower it on anything running
  // off a tired power bank.
#ifndef BADGE_TX_POWER
#define BADGE_TX_POWER WIFI_POWER_11dBm
#endif
  WiFi.setTxPower(BADGE_TX_POWER);
  // Channel 1 everywhere, because nothing negotiates at camp: a badge on a
  // different channel from its conductor is simply deaf, with no error to say
  // why. Overridable only for bench isolation, when two benches within radio
  // range would otherwise count each other's packets.
#ifndef BADGE_WIFI_CHANNEL
#define BADGE_WIFI_CHANNEL 1
#endif
  esp_wifi_set_channel(BADGE_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  setCpuFrequencyMhz(cpuBefore);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[badge] ESP-NOW init FAILED");
    return false;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = BADGE_WIFI_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[badge] add broadcast peer FAILED");
    return false;
  }

  Serial.printf("[badge] ESP-NOW up, channel %d, tx %ddBm, mac %s\n", BADGE_WIFI_CHANNEL,
                (int)WiFi.getTxPower() / 4, WiFi.macAddress().c_str());
  return true;
}

// --- mock DJ (stands in for the microphone conductor) --------------------
static void mockDjFeatures(uint32_t now, float *out) {
  const float beatMs = 60000.0f / (float)MOCK_BPM;
  const float sinceBeat = fmodf((float)now, beatMs);
  const float kick = expf(-sinceBeat / 90.0f);
  const float sinceEighth = fmodf((float)now, beatMs * 0.5f);
  const float hat = expf(-sinceEighth / 35.0f) * 0.6f;

  out[FEAT_BASS] = kick;
  out[FEAT_MID] = 0.35f + 0.35f * sinf((float)now / 1300.0f);
  out[FEAT_TREBLE] = hat + 0.2f + 0.2f * sinf((float)now / 700.0f);
  out[FEAT_ENERGY] = 0.35f + 0.25f * sinf((float)now / 9000.0f) + 0.3f * kick;
  for (int i = 0; i < FEAT_COUNT; i++) {
    if (out[i] < 0.0f) out[i] = 0.0f;
    if (out[i] > 1.0f) out[i] = 1.0f;
  }
}

// --- boot self-test ------------------------------------------------------
// Kept in the shipping firmware on purpose: it is the only way to tell a dead
// backlight from a dead panel from a wrong colour order without instruments,
// and it is readable from across a room or through a webcam.
static void selfTest() {
  struct Card { const char *label; uint8_t r, g, b; uint32_t textColor; };
  static const Card cards[] = {
      {"RED", 255, 0, 0, 0xFFFFFFU},
      {"GREEN", 0, 255, 0, 0x000000U},
      {"BLUE", 0, 0, 255, 0xFFFFFFU},
  };
  display.setTextDatum(middle_center);
  display.setTextSize(3);
  for (const auto &c : cards) {
    display.fillScreen(display.color888(c.r, c.g, c.b));
    display.setTextColor(display.color888((c.textColor >> 16) & 0xFF,
                                          (c.textColor >> 8) & 0xFF,
                                          c.textColor & 0xFF));
    display.drawString(c.label, SCREEN_W / 2, SCREEN_H / 2);
    Serial.printf("[selftest] %s\n", c.label);
    delay(1200);
  }

  // Same red, but written as raw words straight into the sprite buffer and
  // blitted. If this card is red the sprite byte order is right; if it comes
  // out blue, rgbToSprite() needs its swap removed.
  uint16_t *buf = (uint16_t *)canvas.getBuffer();
  const uint16_t red = effect_rgb565(255, 0, 0);
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) buf[i] = red;
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(2);
  canvas.setTextColor(canvas.color888(255, 255, 255));
  canvas.drawString("SPRITE RED", SCREEN_W / 2, SCREEN_H / 2);
  canvas.pushSprite(0, 0);
  Serial.println("[selftest] SPRITE RED (byte-order check)");
  delay(1800);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  prefs.begin("badge", false);
  bootAttempts = prefs.getUInt("bootAttempts", 0) + 1;
  prefs.putUInt("bootAttempts", bootAttempts);
  Serial.printf("\n[badge] boot, reset reason %d (9 = brownout), attempt %lu\n",
                (int)esp_reset_reason(), (unsigned long)bootAttempts);

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  delay(10);

  {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    myId[0] = mac[3];
    myId[1] = mac[4];
    myId[2] = mac[5];
    helloDueMs = 2000 + (chorusIdToTail(myId) % 1800u);
    Serial.printf("[badge] id %02x%02x%02x\n", myId[0], myId[1], myId[2]);
  }
#ifdef BADGE_FORCE_CONDUCTOR
  // Bench builds: nobody is in the room to hold BOOT down at reset.
  isConductor = true;
#else
  isConductor = (digitalRead(PIN_BOOT_BUTTON) == LOW);
#endif

  // The radio comes up FIRST, before the panel and its backlight are drawing
  // anything. Bringing up WiFi is the biggest current spike this board makes,
  // and on a marginal supply it collapses the 3V3 rail -- taking the USB bridge
  // down with it, which from a host looks exactly like a hang. Every milliamp
  // not being spent on a backlight at that instant is headroom.
#ifdef BADGE_SKIP_RADIO
  // Bench builds on a marginal USB supply: sustained transmit collapses the
  // rail on some hosts, and the visuals are what is being looked at.
  Serial.println("[badge] built with BADGE_SKIP_RADIO -- radio off, rendering locally");
  radioUp = false;
#else
  if (bootAttempts > BOOT_ATTEMPTS_BEFORE_GIVING_UP_ON_RADIO) {
    Serial.println("[badge] too many failed boots -- skipping radio, rendering locally");
    radioUp = false;
  } else {
    radioUp = espNowInit();
    if (!radioUp) Serial.println("[badge] no radio -- falling back to local heartbeat");
  }
#endif

  display.init();
  display.setBrightness(255);
  Serial.println("[badge] panel up, backlight GPIO40");

  canvas.setColorDepth(16);
  canvas.setPsram(false);  // sprite in internal RAM; PSRAM writes are too slow
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("[badge] FATAL: could not allocate 240x240 sprite");
    while (true) delay(1000);
  }
  canvas.fillSprite(0);

  selfTest();
  canvas.fillSprite(0);  // effects only write inside the circle; clear the rest
  effectsInit();
  monSelectForThisBadge();
  defaultShader = resolveDefaultShader();
  activeShader = defaultShader;
  conductorShader = defaultShader;
  Serial.printf("[badge] default effect %u (%s)\n", (unsigned)defaultShader, effects_by_index(defaultShader)->name);
#ifdef BADGE_LOCK_EFFECT
  // Bag builds: every badge wears its own crest no matter what the conductor's
  // shader byte says. The features still come from the conductor.
  activeShader = (uint8_t)BADGE_LOCK_EFFECT;
  Serial.printf("[badge] effect locked to %d (%s)\n", (int)activeShader,
                effects_by_index(activeShader)->name);
#endif

#ifdef BADGE_PHONE_LINK
  // BLE shares the radio with ESP-NOW, so it comes up last, after the spike of
  // WiFi bring-up has passed and the panel is already drawing.
  phoneLinkUp = phoneLinkBegin();
#endif

  Serial.printf("[badge] role: %s\n", isConductor ? "CONDUCTOR (mock DJ)" : "RECEIVER");
  Serial.printf("[badge] free heap %u, free psram %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

// --- the button on the back --------------------------------------------
// Held at reset it picks the conductor role (see setup). While the badge is
// running it is the only control a wearer has, so it does the two things
// somebody actually wants at 2am in a field:
//
//   tap        next effect, and stay on it -- the same pin a phone would set
//   hold 1.2s  let go, follow the leader again
//
// A tap pins deliberately. A badge whose wearer picked a visual should keep it
// even though the conductor is still shouting a different shader thirty times a
// second; anything else makes the button look broken.
static constexpr uint32_t BUTTON_HOLD_MS = 1200;
static const char *toastText = nullptr;
static uint32_t toastUntilMs = 0;

static void toast(const char *text, uint32_t ms = 1500) {
  toastText = text;
  toastUntilMs = millis() + ms;
}

static void pollButton(uint32_t now) {
  static bool wasDown = false;
  static uint32_t downAtMs = 0;
  static bool heldFired = false;
  const bool down = digitalRead(PIN_BOOT_BUTTON) == LOW;

  if (down && !wasDown) {
    downAtMs = now;
    heldFired = false;
  } else if (down && !heldFired && (now - downAtMs) >= BUTTON_HOLD_MS) {
    // Fire the hold as soon as it is long enough, not on release: the wearer
    // gets the feedback while their thumb is still on the button.
    heldFired = true;
    pinned = false;
    pinUntilMs = 0;
    toast("following leader");
    Serial.println("[badge] button: hold -> release pin, follow the leader");
  } else if (!down && wasDown && !heldFired) {
    const uint32_t heldMs = now - downAtMs;
    if (heldMs >= 40) {  // anything shorter is contact bounce, not a person
      const uint8_t from = pinned ? pinnedShader : activeShader;
      pinnedShader = (uint8_t)((from + 1) % effects_count);
      pinUntilMs = 0;
      pinned = true;
      toast(effects_by_index(pinnedShader)->name);
      Serial.printf("[badge] button: tap -> effect %u (%s)\n", (unsigned)pinnedShader,
                    effects_by_index(pinnedShader)->name);
    }
  }
  wasDown = down;
}

// --- roster beacon -------------------------------------------------------
// Every badge says who it is and what it is showing, so the leader can offer a
// list to a phone instead of the phone having to know the swarm in advance.
// Once every HELLO_INTERVAL_MS, phase-offset by MAC so the swarm does not all
// speak on the same tick.
static constexpr uint32_t HELLO_INTERVAL_MS = 2000;

static void sendHello(uint32_t now, uint32_t fps, uint32_t rxPerSec, bool hearing) {
  ChorusHello h = {};
  memcpy(h.magic, CHORUS_HELLO_MAGIC, 4);
  memcpy(h.id, myId, 3);
  h.seq = helloSeq++;
  h.shader = activeShader;
  h.flags = (uint8_t)((pinned ? HELLO_PINNED : 0) | (hearing ? HELLO_HEARING : 0) |
                      (isConductor ? HELLO_CONDUCTING : 0) |
                      ((now < identifyUntilMs) ? HELLO_IDENTIFYING : 0));
  h.fps = (uint8_t)(fps > 255 ? 255 : fps);
  h.crest = (uint8_t)myCrest;
  h.uptimeS = (uint16_t)(now / 1000u);
  h.hop = 0;
  h.rxPerSec = (uint8_t)(rxPerSec > 255 ? 255 : rxPerSec);
  broadcastRaw(&h, sizeof(h));
  helloSent++;
}

void loop() {
  const uint32_t now = millis();
  pollSerial();
  pollButton(now);

  // --- conductor: analyse (for now, pretend to) and broadcast ---
  static uint32_t lastSendMs = 0;
  static uint16_t seq = 0;
  float target[FEAT_COUNT] = {0, 0, 0, 0};
  bool heard = false;

  if (isConductor) {
    mockDjFeatures(now, target);
    if (now - lastSendMs >= CONDUCTOR_INTERVAL_MS) {
      lastSendMs = now;
      ChorusPacket pkt;
      memcpy(pkt.magic, CHORUS_MAGIC, 4);
      pkt.seq = seq++;
      pkt.hop = 0;
      pkt.shader = conductorShader;
      for (int i = 0; i < FEAT_COUNT; i++) pkt.features[i] = target[i];
      broadcast(pkt);
    }
  } else if (phoneLinkUp && !heardRecently(now) && phoneLinkFresh(now)) {
    // Nothing on the air and a phone is feeding us: this badge becomes the
    // conductor and puts the phone's analysis on the air for everyone else.
    uint8_t phoneBeat = 0, phoneShader = 0;
    phoneLinkRead(target, &phoneBeat, &phoneShader);
    // The phone ran a real onset detector -- median+MAD spectral flux with
    // hysteresis and a refractory period -- and told us the answer. Use it.
    // Re-deriving beat from an energy jump here would throw away the good
    // signal and substitute the exact heuristic this design exists to avoid.
    phoneOnset = phoneBeat != 0;
    activeShader = phoneShader;
    activeSource = SRC_PHONE;
    heard = true;  // drives `presence`; the phone is our conductor

    static uint32_t lastPhoneTxMs = 0;
    static uint16_t phoneSeq = 0;
    if (radioUp && (now - lastPhoneTxMs) >= CONDUCTOR_INTERVAL_MS) {
      lastPhoneTxMs = now;
      ChorusPacket pkt;
      memcpy(pkt.magic, CHORUS_MAGIC, 4);
      pkt.seq = phoneSeq++;
      pkt.hop = 0;
      pkt.shader = phoneShader;
      for (int i = 0; i < FEAT_COUNT; i++) pkt.features[i] = target[i];
      broadcast(pkt);
    }
  } else if (!radioUp) {
    mockDjFeatures(now, target);
  } else {
    uint32_t lastMs;
    uint8_t packetShader;
    portENTER_CRITICAL(&featureMux);
    for (int i = 0; i < FEAT_COUNT; i++) target[i] = rxFeatures[i];
    lastMs = rxLastMs;
    packetShader = rxShader;
    portEXIT_CRITICAL(&featureMux);

    // No conductor in earshot: decay toward stillness rather than freezing on
    // the last packet, so a badge that walks out of range exhales.
    heard = rxSeen && (now - lastMs) <= FEATURE_STALE_MS;
    activeSource = heard ? SRC_ESPNOW : SRC_NONE;
#ifndef BADGE_LOCK_EFFECT
    // "Everyone switch to 3" while a conductor is talking; once it has been
    // gone a while, settle back onto this badge's own default look.
    if (heard) activeShader = packetShader;
    else if (now - lastMs > DEFAULT_REVERT_MS) activeShader = defaultShader;
#endif
  }

  // A pin -- from the button on the back or from a phone by way of the leader
  // -- is the last word on what this badge shows. It sits outside the
  // source-selection branches above on purpose, so it wins for a conductor, a
  // receiver and a BADGE_LOCK_EFFECT bag build alike: the lock chooses a
  // badge's default, it does not forbid a human from changing their mind.
  if (pinned && pinUntilMs != 0 && (int32_t)(now - pinUntilMs) >= 0) {
    pinned = false;
    pinUntilMs = 0;
  }
  if (pinned) activeShader = pinnedShader;

  // Drain queued relays here, in loop context, where esp_now_send() is safe.
  while (relayTail != relayHead) {
    broadcast(relayQueue[relayTail]);
    relayTail = (uint8_t)((relayTail + 1) % RELAY_QUEUE_LEN);
    relayCount++;
  }

  // Same rule for commands and beacons passing through.
  while (auxTail != auxHead) {
    broadcastRaw(auxQueue[auxTail].bytes, auxQueue[auxTail].len);
    auxTail = (uint8_t)((auxTail + 1) % AUX_QUEUE_LEN);
  }

  // Say who we are, on our own clock, phase-offset by MAC so thirty badges do
  // not all beacon on the same tick. A roll call can bring this forward.
  if (radioUp && (int32_t)(now - helloDueMs) >= 0) {
    sendHello(now, helloFps, helloRxPerSec, heard);
    helloDueMs = now + HELLO_INTERVAL_MS;
  }

  // Backlight changes are a panel call, so they happen here rather than in the
  // receive callback that asked for them.
  if (requestedBrightness != appliedBrightness) {
    appliedBrightness = requestedBrightness;
    display.setBrightness(appliedBrightness);
  }

  // Presence, not smoothing: a badge that can still hear the conductor shows
  // what it was told; one that has walked out of range exhales over ~600ms.
  const bool hearing = isConductor || !radioUp || heard;
  presence += ((hearing ? 1.0f : 0.0f) - presence) * 0.05f;
  for (int i = 0; i < FEAT_COUNT; i++) shown[i] = target[i] * presence;

  // The conductor expresses an onset as a single-packet jump in energy, then
  // releases on a designed curve, so that jump is the beat and the value that
  // follows it is already the envelope.
  static float prevEnergy = 0.0f;
  EffectInput in;
  in.bass = shown[FEAT_BASS];
  in.mid = shown[FEAT_MID];
  in.treble = shown[FEAT_TREBLE];
  in.energy = shown[FEAT_ENERGY];
  in.time_ms = now;
  // A conductor on the air can only express an onset by shaping energy, so for
  // ESP-NOW we still infer it. A phone says so explicitly, and that is strictly
  // better information -- prefer it whenever it is what is driving us.
  if (activeSource == SRC_PHONE) {
    in.beat = phoneOnset ? 1 : 0;
    phoneOnset = false;  // an onset is one frame, not a level
  } else {
    in.beat = (shown[FEAT_ENERGY] - prevEnergy) > 0.25f ? 1 : 0;
  }
  in.beat_env = shown[FEAT_ENERGY];
  prevEnergy = shown[FEAT_ENERGY];

  const Effect *effect = effects_by_index(activeShader);
  effect->render((uint16_t *)canvas.getBuffer(), &in);

  // Bring-up HUD. Cheap, and it makes a photograph of the screen into a
  // readable status report.
  static uint32_t fps = 0;
  canvas.setTextDatum(middle_center);
  canvas.setTextSize(1);
  canvas.setTextColor(canvas.color888(255, 255, 255));
  const char *hudRole = isConductor                    ? "CONDUCTOR"
                        : (activeSource == SRC_PHONE)  ? "PHONE"
                        : !radioUp                     ? "NO RADIO"
                        : (activeSource == SRC_ESPNOW) ? "RECEIVER"
                                                       : "LISTENING";
  canvas.drawString(hudRole, SCREEN_W / 2, 60);
  char hud[40];
  snprintf(hud, sizeof(hud), "%lu fps  rx:%lu", (unsigned long)fps, (unsigned long)rxCount);
  canvas.drawString(hud, SCREEN_W / 2, 180);
  canvas.drawString(effects_by_index(activeShader)->name, SCREEN_W / 2, 200);
  if (phoneLinkUp && phoneLinkConnected() && activeSource != SRC_PHONE) {
    canvas.drawString("phone standby", SCREEN_W / 2, 212);
  }
  if (pinned) canvas.drawString("pinned", SCREEN_W / 2, 224);

  // Identify: pulse a white ring so one badge stands out in a crowd of thirty
  // in the dark. A ring rather than a filled screen -- the effect stays legible
  // underneath, so it is obvious the badge is alive and merely answering.
  if (now < identifyUntilMs) {
    const float phase = (float)((now % 500) / 500.0f);
    const int r = (int)(SCREEN_W / 2 - 4 - phase * 24.0f);
    const uint8_t v = (uint8_t)(255.0f * (1.0f - phase));
    canvas.drawCircle(SCREEN_W / 2, SCREEN_H / 2, r, canvas.color888(v, v, v));
    canvas.drawCircle(SCREEN_W / 2, SCREEN_H / 2, r - 1, canvas.color888(v, v, v));
  }

  // A tap on the back button has to say something, or it looks broken.
  if (toastText && now < toastUntilMs) {
    canvas.setTextSize(2);
    canvas.drawString(toastText, SCREEN_W / 2, SCREEN_H / 2);
    canvas.setTextSize(1);
  }

  canvas.pushSprite(0, 0);

  static uint32_t frames = 0, lastReportMs = 0;
  frames++;
  if (now - lastReportMs >= 1000) {
    fps = frames * 1000 / (now - lastReportMs);
    // Report what is actually driving this badge, not just how it booted: a
    // badge conducting from a phone is neither a CONDUCTOR nor a RECEIVER, and
    // labelling it wrong is how a log lies to whoever reads it next.
    const char *sourceName = isConductor                    ? "CONDUCTOR"
                             : (activeSource == SRC_PHONE)  ? "PHONE-LED"
                             : (activeSource == SRC_ESPNOW) ? "RECEIVER"
                                                            : "IDLE";
    Serial.printf("[badge] %s %lu fps | bass %.2f mid %.2f treble %.2f energy %.2f | rx %lu relay %lu | fx %s\n",
                  sourceName, (unsigned long)fps,
                  shown[FEAT_BASS], shown[FEAT_MID], shown[FEAT_TREBLE],
                  shown[FEAT_ENERGY], (unsigned long)rxCount, (unsigned long)relayCount,
                  effects_by_index(activeShader)->name);
    if (txOk || txFail) {
      Serial.printf("[badge] tx ok %lu fail %lu | resyncs %lu | boot attempts %lu\n",
                    (unsigned long)txOk, (unsigned long)txFail,
                    (unsigned long)resyncCount, (unsigned long)bootAttempts);
    }
    if (relayDropped) Serial.printf("[badge] relay queue dropped %lu\n", (unsigned long)relayDropped);
    if (auxDropped) Serial.printf("[badge] command queue dropped %lu\n", (unsigned long)auxDropped);
    if (cmdCount || helloSent) {
      Serial.printf("[badge] id %02x%02x%02x | commands %lu | hellos %lu | %s\n", myId[0], myId[1],
                    myId[2], (unsigned long)cmdCount, (unsigned long)helloSent,
                    pinned ? "PINNED" : "following");
    }

    // Neighbour table: who this badge is hearing, and at what hop distance.
    // Reported every second alongside the counters so a multi-bench test can be
    // read straight off the serial line without correlating logs afterwards.
    char neighbours[200];
    int used = 0;
    neighbours[0] = '\0';
    portENTER_CRITICAL(&featureMux);
    for (int i = 0; i < MAX_SOURCES && used < (int)sizeof(neighbours) - 40; i++) {
      if (!sources[i].used) continue;
      used += snprintf(neighbours + used, sizeof(neighbours) - used, " %02X%02X%02X:%lu(h%u)",
                       sources[i].mac[3], sources[i].mac[4], sources[i].mac[5],
                       (unsigned long)sources[i].count, (unsigned)sources[i].lastHop);
    }
    uint32_t hops[CHORUS_MAX_HOP + 2];
    memcpy(hops, rxByHop, sizeof(hops));
    portEXIT_CRITICAL(&featureMux);
    if (used) {
      Serial.printf("[badge] heard%s | by hop: %lu/%lu/%lu/%lu/%lu\n", neighbours,
                    (unsigned long)hops[0], (unsigned long)hops[1], (unsigned long)hops[2],
                    (unsigned long)hops[3], (unsigned long)hops[4]);
    }
    if (phoneLinkUp) {
      const uint8_t role = (activeSource == SRC_PHONE)    ? PHONE_ROLE_PHONE_LED
                           : (activeSource == SRC_ESPNOW) ? PHONE_ROLE_RECEIVER
                                                          : PHONE_ROLE_IDLE;
      // Rates, as the contract says -- not totals. Cumulative counters cast to
      // uint16 wrap every ~35 minutes at 31 pkt/s, and a phone showing a
      // rewinding "rate" is worse than showing nothing.
      static uint32_t prevRx = 0, prevTx = 0;
      const uint16_t rxRate = (uint16_t)(rxCount - prevRx);
      const uint16_t txRate = (uint16_t)(txOk - prevTx);
      prevRx = rxCount;
      prevTx = txOk;
      phoneLinkPublishStatus(role, activeSource == SRC_ESPNOW, rxRate, txRate);
    }
    // The rx rate the roster beacon reports, measured over this same second.
    static uint32_t prevRxForHello = 0;
    helloRxPerSec = rxCount - prevRxForHello;
    prevRxForHello = rxCount;
    helloFps = fps;

    frames = 0;
    lastReportMs = now;
    if (bootAttempts && now > 3000) {
      bootAttempts = 0;  // rendering steadily; this boot was a good one
      prefs.putUInt("bootAttempts", 0);
    }
  }
}
