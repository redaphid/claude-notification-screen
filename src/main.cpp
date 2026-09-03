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

static RoundBadgeDisplay display;
static LGFX_Sprite canvas(&display);

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint32_t CONDUCTOR_INTERVAL_MS = 33;  // ~30Hz
static constexpr uint32_t FEATURE_STALE_MS = 300;
static constexpr uint16_t MOCK_BPM = 118;

static bool isConductor = false;
static bool radioUp = false;

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

static float smoothed[FEAT_COUNT] = {0, 0, 0, 0};
static constexpr float ATTACK = 0.65f;
static constexpr float RELEASE = 0.12f;

// --- plasma (bring-up visual) -------------------------------------------
// Temporary: proves the render path and the frame budget on real hardware.
// The visuals stream is porting the real effects against effects/effect.h;
// this gets swapped for those.
static uint8_t sinTab[256];
static uint8_t *radTab = nullptr;
static uint16_t palette[256];
static int16_t rowStart[SCREEN_H], rowEnd[SCREEN_H];
static uint8_t colTerm[SCREEN_W], rowTerm[SCREEN_H];

static inline uint16_t rgbToSprite(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  return (uint16_t)((c >> 8) | (c << 8));  // LovyanGFX sprites store swapped 565
}

static void plasmaInit() {
  for (int i = 0; i < 256; i++) {
    float s = sinf((float)i * 2.0f * (float)M_PI / 256.0f);
    sinTab[i] = (uint8_t)((s * 0.5f + 0.5f) * 85.0f);  // three terms sum to <= 255
  }

  radTab = (uint8_t *)ps_malloc(SCREEN_W * SCREEN_H);
  if (!radTab) {
    radTab = (uint8_t *)malloc(SCREEN_W * SCREEN_H);
    Serial.println(radTab ? "[badge] radial LUT in internal RAM (no PSRAM)"
                          : "[badge] radial LUT alloc FAILED, plasma will be flat");
  } else {
    Serial.println("[badge] radial LUT in PSRAM");
  }

  for (int y = 0; y < SCREEN_H; y++) {
    float dy = (float)y - 119.5f;
    float half = sqrtf(fmaxf(0.0f, 120.0f * 120.0f - dy * dy));
    rowStart[y] = (int16_t)ceilf(119.5f - half);
    rowEnd[y] = (int16_t)floorf(119.5f + half);
    if (rowStart[y] < 0) rowStart[y] = 0;
    if (rowEnd[y] > SCREEN_W - 1) rowEnd[y] = SCREEN_W - 1;
    if (radTab) {
      for (int x = 0; x < SCREEN_W; x++) {
        float dx = (float)x - 119.5f;
        radTab[y * SCREEN_W + x] = (uint8_t)(sqrtf(dx * dx + dy * dy) * 2.0f);
      }
    }
  }
}

// Rebuilt once per frame -- 256 iterations instead of 57600, so brightness and
// palette rotation are effectively free.
static void buildPalette(uint8_t hueOffset, float brightness) {
  if (brightness < 0.0f) brightness = 0.0f;
  if (brightness > 1.0f) brightness = 1.0f;
  const int v = (int)(brightness * 255.0f);
  for (int i = 0; i < 256; i++) {
    int h = (i + hueOffset) & 0xFF;
    int region = h / 43;
    int rem = (h - region * 43) * 6;
    int p = 0;
    int q = (v * (255 - rem)) >> 8;
    int t = (v * rem) >> 8;
    int r, g, b;
    switch (region) {
      case 0:  r = v; g = t; b = p; break;
      case 1:  r = q; g = v; b = p; break;
      case 2:  r = p; g = v; b = t; break;
      case 3:  r = p; g = q; b = v; break;
      case 4:  r = t; g = p; b = v; break;
      default: r = v; g = p; b = q; break;
    }
    palette[i] = rgbToSprite((uint8_t)r, (uint8_t)g, (uint8_t)b);
  }
}

static void plasmaRender(uint16_t *out) {
  // Phase accumulators rather than phases derived from millis(): when the music
  // speeds the field up, it accelerates instead of jumping.
  static float accX = 0, accY = 0, accR = 0, accHue = 0;
  const float bass = smoothed[FEAT_BASS];
  const float mid = smoothed[FEAT_MID];
  const float treble = smoothed[FEAT_TREBLE];
  const float energy = smoothed[FEAT_ENERGY];

  accX += 0.8f + energy * 2.5f;
  accY += 0.6f + mid * 2.0f;
  accR += 1.2f + bass * 4.0f;
  accHue += 0.35f + treble * 3.0f;
  if (accX > 65536.0f) accX -= 65536.0f;
  if (accY > 65536.0f) accY -= 65536.0f;
  if (accR > 65536.0f) accR -= 65536.0f;
  if (accHue > 65536.0f) accHue -= 65536.0f;

  buildPalette((uint8_t)accHue, 0.35f + 0.65f * bass);

  const uint8_t fx = (uint8_t)(2 + (int)(bass * 3.0f));
  const uint8_t fy = (uint8_t)(2 + (int)(mid * 3.0f));
  const uint8_t phX = (uint8_t)accX;
  const uint8_t phY = (uint8_t)accY;
  const uint8_t phR = (uint8_t)accR;

  for (int x = 0; x < SCREEN_W; x++) colTerm[x] = sinTab[(uint8_t)(x * fx + phX)];
  for (int y = 0; y < SCREEN_H; y++) rowTerm[y] = sinTab[(uint8_t)(y * fy + phY)];

  for (int y = 0; y < SCREEN_H; y++) {
    const int x0 = rowStart[y], x1 = rowEnd[y];
    uint16_t *dst = out + y * SCREEN_W + x0;
    const uint8_t ry = rowTerm[y];
    if (radTab) {
      const uint8_t *rad = radTab + y * SCREEN_W + x0;
      for (int x = x0; x <= x1; x++) {
        *dst++ = palette[(uint8_t)(colTerm[x] + ry + sinTab[(uint8_t)(*rad++ + phR)])];
      }
    } else {
      for (int x = x0; x <= x1; x++) *dst++ = palette[(uint8_t)(colTerm[x] + ry)];
    }
  }
}

// --- ESP-NOW -------------------------------------------------------------
static void broadcast(const ChorusPacket &pkt) {
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
  const uint16_t red = rgbToSprite(255, 0, 0);
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
  Serial.printf("\n[badge] boot, reset reason %d (9 = brownout)\n", (int)esp_reset_reason());

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  delay(10);
  isConductor = (digitalRead(PIN_BOOT_BUTTON) == LOW);

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
  canvas.fillSprite(0);  // plasma only writes inside the circle; clear the rest
  plasmaInit();
  radioUp = espNowInit();
  if (!radioUp) {
    // A badge with no radio still has a screen. It renders its own idle
    // heartbeat rather than going dark in someone's hand.
    Serial.println("[badge] no radio -- falling back to local heartbeat");
  }

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
    portEXIT_CRITICAL(&featureMux);

    // No conductor in earshot: decay toward stillness rather than freezing on
    // the last packet, so a badge that walks out of range exhales.
    if (!rxSeen || (now - lastMs) > FEATURE_STALE_MS) {
      for (int i = 0; i < FEAT_COUNT; i++) target[i] = 0.0f;
    }
  }

  // Drain queued relays here, in loop context, where esp_now_send() is safe.
  while (relayTail != relayHead) {
    broadcast(relayQueue[relayTail]);
    relayTail = (uint8_t)((relayTail + 1) % RELAY_QUEUE_LEN);
    relayCount++;
  }

  for (int i = 0; i < FEAT_COUNT; i++) {
    const float a = (target[i] > smoothed[i]) ? ATTACK : RELEASE;
    smoothed[i] += (target[i] - smoothed[i]) * a;
  }

  plasmaRender((uint16_t *)canvas.getBuffer());

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

  canvas.pushSprite(0, 0);

  static uint32_t frames = 0, lastReportMs = 0;
  frames++;
  if (now - lastReportMs >= 1000) {
    fps = frames * 1000 / (now - lastReportMs);
    Serial.printf("[badge] %s %lu fps | bass %.2f mid %.2f treble %.2f energy %.2f | rx %lu relay %lu\n",
                  isConductor ? "CONDUCTOR" : "RECEIVER", (unsigned long)fps,
                  smoothed[FEAT_BASS], smoothed[FEAT_MID], smoothed[FEAT_TREBLE],
                  smoothed[FEAT_ENERGY], (unsigned long)rxCount, (unsigned long)relayCount);
    if (relayDropped) Serial.printf("[badge] relay queue dropped %lu\n", (unsigned long)relayDropped);
    frames = 0;
    lastReportMs = now;
  }
}
