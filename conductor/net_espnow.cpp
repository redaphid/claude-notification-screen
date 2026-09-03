#include "net_espnow.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static volatile uint32_t s_echoes = 0;
static volatile uint8_t s_lastEchoHop = 0;

// Arduino core 2.0.11's esp_now.h declares:
//   typedef void (*esp_now_recv_cb_t)(const uint8_t *mac_addr,
//                                     const uint8_t *data, int data_len);
// Three arguments. Core 3.x changed this to take an esp_now_recv_info_t*; that
// signature will NOT compile here. Verified against the installed header, not
// remembered.
static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
  if (!chorusPacketValid(data, len)) return;
  ChorusPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.hop == 0) return;  // another conductor would be a bug; ignore originals
  s_lastEchoHop = p.hop;
  s_echoes++;
}

static void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  (void)status;  // broadcast frames are unacknowledged; status is not meaningful
}

uint32_t ChorusRadio::echoesHeard() { return s_echoes; }
uint8_t ChorusRadio::lastEchoHop() { return s_lastEchoHop; }

bool ChorusRadio::begin() {
  // Radio bring-up is this board's biggest current spike. On a marginal supply
  // it drops the 3V3 rail hard enough to take the USB bridge down with the
  // ESP32, which presents to the host as a hang inside WiFi.mode() rather than
  // as anything resembling a power problem. Slow the core down across the spike
  // and restore it afterwards.
  uint32_t origMhz = getCpuFrequencyMhz();
  bool throttled = origMhz > RADIO_BRINGUP_CPU_MHZ;
  if (throttled) {
    Serial.printf("[radio] throttling %u -> %d MHz across bring-up\n", (unsigned)origMhz,
                  RADIO_BRINGUP_CPU_MHZ);
    setCpuFrequencyMhz(RADIO_BRINGUP_CPU_MHZ);
  }

  bool ok = bringUp();

  if (throttled) {
    setCpuFrequencyMhz(origMhz);
    Serial.printf("[radio] restored %u MHz\n", (unsigned)origMhz);
  }
  _ready = ok;
  return ok;
}

bool ChorusRadio::bringUp() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  // Power save inserts tens of milliseconds of latency on an otherwise idle
  // radio, which is most of the budget this whole design is trying to protect.
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Reduced TX power, for the same rail-collapse reason. Every transmit is a
  // current spike; at 30 Hz forever, the peak draw matters more here than the
  // last few dB of range, which the mesh's hop count buys back anyway.
  esp_wifi_set_max_tx_power(RADIO_TX_POWER_QDBM);

  esp_err_t err = esp_wifi_set_channel(CHORUS_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) {
    Serial.printf("[radio] esp_wifi_set_channel(%d) failed: %d\n", CHORUS_WIFI_CHANNEL, (int)err);
  }

  // begin() is retried from the main loop after a failure, so this has to be
  // safe to call more than once.
  static bool nowInited = false;
  if (!nowInited) {
    if (esp_now_init() != ESP_OK) {
      Serial.println("[radio] esp_now_init failed");
      return false;
    }
    nowInited = true;
  }

  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcast, 6);
  peer.channel = 0;  // 0 == whatever channel the interface is already on
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  err = esp_now_add_peer(&peer);
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[radio] esp_now_add_peer failed: %d\n", (int)err);
    return false;
  }

  Serial.printf("[radio] ESP-NOW up on channel %d at %d/4 dBm, MAC %s, %u-byte packets\n",
                CHORUS_WIFI_CHANNEL, RADIO_TX_POWER_QDBM, WiFi.macAddress().c_str(),
                (unsigned)sizeof(ChorusPacket));
  return true;
}

bool ChorusRadio::broadcast(uint8_t shader, float bass, float mid, float treble, float energy) {
  ChorusPacket p;
  memcpy(p.magic, CHORUS_MAGIC, 4);
  p.seq = _seq++;
  p.hop = 0;  // straight from the conductor; relays increment it
  p.shader = shader;
  p.features[FEAT_BASS] = bass;
  p.features[FEAT_MID] = mid;
  p.features[FEAT_TREBLE] = treble;
  p.features[FEAT_ENERGY] = energy;

  esp_err_t err = esp_now_send(kBroadcast, (const uint8_t *)&p, sizeof(p));
  if (err != ESP_OK) {
    _failures++;
    return false;
  }
  _sent++;
  return true;
}
