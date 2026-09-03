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
#include <esp_system.h>
#include <math.h>

#include "chorus_packet.h"
#include "display.h"
#include "effects.h"

static RoundBadgeDisplay display;
static LGFX_Sprite canvas(&display);

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint32_t CONDUCTOR_INTERVAL_MS = 33;  // ~30Hz
static constexpr uint32_t FEATURE_STALE_MS = 300;
static constexpr uint16_t MOCK_BPM = 118;

static bool isConductor = false;
static bool radioUp = false;

// Survives a reset, so a badge can notice it is caught in a boot loop. If the
// radio brings the rail down three times running -- a tired power bank, a thin
// cable -- the next boot skips it entirely and the badge renders locally. A
// giveaway badge that looks dead is worse than one that is merely alone.
RTC_DATA_ATTR static uint32_t bootAttempts = 0;
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

static void effectsInit() {
  for (int i = 0; i < effects_count; i++) {
    if (effects_all[i]->init) effects_all[i]->init();
  }
  Serial.printf("[badge] %d effects registered\n", effects_count);
}

// --- ESP-NOW -------------------------------------------------------------
static void broadcast(const ChorusPacket &pkt) {
  // esp_now_send() reads driver state that only exists after a successful
  // init, so calling it without one is a null dereference, not a no-op. A
  // conductor whose radio failed must keep rendering, not panic.
  if (!radioUp) return;
  esp_now_send(BROADCAST_ADDR, (const uint8_t *)&pkt, sizeof(pkt));
}

static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (!chorusPacketValid(data, len)) return;
  ChorusPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  portENTER_CRITICAL(&featureMux);
  const bool fresh = !rxSeen || chorusSeqNewer(pkt.seq, rxLastSeq);
  if (fresh) {
    rxLastSeq = pkt.seq;
    rxSeen = true;
    rxLastMs = millis();
    rxCount++;
    for (int i = 0; i < FEAT_COUNT; i++) rxFeatures[i] = pkt.features[i];
    rxShader = pkt.shader;
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
  WiFi.setSleep(false);  // ESP-NOW receive must not miss packets to modem sleep
  WiFi.setTxPower(WIFI_POWER_11dBm);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  setCpuFrequencyMhz(cpuBefore);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[badge] ESP-NOW init FAILED");
    return false;
  }
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 1;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[badge] add broadcast peer FAILED");
    return false;
  }

  Serial.printf("[badge] ESP-NOW up, channel 1, tx %ddBm, mac %s\n",
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
  bootAttempts++;
  Serial.printf("\n[badge] boot, reset reason %d (9 = brownout), attempt %lu\n",
                (int)esp_reset_reason(), (unsigned long)bootAttempts);

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  delay(10);
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

  Serial.printf("[badge] role: %s\n", isConductor ? "CONDUCTOR (mock DJ)" : "RECEIVER");
  Serial.printf("[badge] free heap %u, free psram %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

void loop() {
  const uint32_t now = millis();

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
      pkt.shader = 0;
      for (int i = 0; i < FEAT_COUNT; i++) pkt.features[i] = target[i];
      broadcast(pkt);
    }
  } else if (!radioUp) {
    mockDjFeatures(now, target);
  } else {
    uint32_t lastMs;
    portENTER_CRITICAL(&featureMux);
    for (int i = 0; i < FEAT_COUNT; i++) target[i] = rxFeatures[i];
    lastMs = rxLastMs;
    activeShader = rxShader;  // "everyone switch to 3"
    portEXIT_CRITICAL(&featureMux);

    // No conductor in earshot: decay toward stillness rather than freezing on
    // the last packet, so a badge that walks out of range exhales.
    heard = rxSeen && (now - lastMs) <= FEATURE_STALE_MS;
  }

  // Drain queued relays here, in loop context, where esp_now_send() is safe.
  while (relayTail != relayHead) {
    broadcast(relayQueue[relayTail]);
    relayTail = (uint8_t)((relayTail + 1) % RELAY_QUEUE_LEN);
    relayCount++;
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
  in.beat = (shown[FEAT_ENERGY] - prevEnergy) > 0.25f ? 1 : 0;
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
  canvas.drawString(isConductor ? "CONDUCTOR" : (radioUp ? "RECEIVER" : "NO RADIO"), SCREEN_W / 2, 60);
  char hud[40];
  snprintf(hud, sizeof(hud), "%lu fps  rx:%lu", (unsigned long)fps, (unsigned long)rxCount);
  canvas.drawString(hud, SCREEN_W / 2, 180);
  canvas.drawString(effects_by_index(activeShader)->name, SCREEN_W / 2, 200);

  canvas.pushSprite(0, 0);

  static uint32_t frames = 0, lastReportMs = 0;
  frames++;
  if (now - lastReportMs >= 1000) {
    fps = frames * 1000 / (now - lastReportMs);
    Serial.printf("[badge] %s %lu fps | bass %.2f mid %.2f treble %.2f energy %.2f | rx %lu relay %lu\n",
                  isConductor ? "CONDUCTOR" : "RECEIVER", (unsigned long)fps,
                  shown[FEAT_BASS], shown[FEAT_MID], shown[FEAT_TREBLE],
                  shown[FEAT_ENERGY], (unsigned long)rxCount, (unsigned long)relayCount);
    if (relayDropped) Serial.printf("[badge] relay queue dropped %lu\n", (unsigned long)relayDropped);
    frames = 0;
    lastReportMs = now;
    if (bootAttempts && now > 3000) {
      bootAttempts = 0;  // rendering steadily; this boot was a good one
    }
  }
}
