// The phone-to-badge BLE contract.
//
// A phone cannot speak ESP-NOW -- no browser can -- so when there is no
// conductor in the room, a phone becomes one: it listens with its own
// microphone, does the analysis in the browser, and writes features to any
// badge over BLE. That badge relays them into the swarm over ESP-NOW, which
// every other badge already understands. One phone, one truth, same as one mic.
//
// This header is the contract between the firmware and the web page. Keep the
// two in step; the page reads these same UUIDs and this same byte layout.
#pragma once

#include <stdint.h>

// A badge advertises as "Chorus-XXXX", where XXXX is the last two bytes of its
// MAC in hex, so a human choosing from a list can tell two badges apart.
#define PHONE_LINK_NAME_PREFIX "Chorus-"

#define PHONE_LINK_SERVICE_UUID "c8a0f100-0451-4000-b000-63726e730001"
// Phone -> badge. Write-without-response: at 30Hz, waiting for an ack per packet
// would cost more than dropping one, and a dropped feature frame is obsolete in
// 33ms anyway. Same reasoning as ESP-NOW broadcast having no ACK.
#define PHONE_LINK_FEATURES_UUID "c8a0f101-0451-4000-b000-63726e730001"
// Badge -> phone. Notify, ~1Hz: lets the page show whether it is actually
// conducting, or whether a real leader is present and it is being ignored.
#define PHONE_LINK_STATUS_UUID "c8a0f102-0451-4000-b000-63726e730001"

// 8 bytes. Deliberately not the 24-byte ChorusPacket: features are quantised to
// a byte each because a phone sends over a link with a connection interval, and
// nobody can see the difference between 1/255 steps of "bass" on a 240px disc.
struct __attribute__((packed)) PhoneFeatureFrame {
  uint8_t bass;    // 0..255 maps to 0..1
  uint8_t mid;
  uint8_t treble;
  uint8_t energy;
  uint8_t beat;    // 1 only on the frame an onset fired
  uint8_t shader;  // which effect the swarm should show
  uint16_t seq;    // phone-side counter; wraps, used to spot a stalled phone
};

static_assert(sizeof(PhoneFeatureFrame) == 8, "PhoneFeatureFrame is a wire contract");

// What the badge tells the phone about itself.
enum PhoneLinkRole : uint8_t {
  PHONE_ROLE_IDLE = 0,       // no conductor anywhere, and no phone feeding us
  PHONE_ROLE_PHONE_LED = 1,  // this badge is conducting from the phone's audio
  PHONE_ROLE_RECEIVER = 2,   // a real conductor is on the air; phone is standby
};

struct __attribute__((packed)) PhoneStatusFrame {
  uint8_t role;         // PhoneLinkRole
  uint8_t espnowHeard;  // 1 if a non-phone conductor has been heard recently
  uint16_t rxRate;      // ESP-NOW packets/sec this badge is receiving
  uint16_t txRate;      // packets/sec this badge is putting on the air
  uint16_t frames;      // phone frames accepted since connect
};

static_assert(sizeof(PhoneStatusFrame) == 8, "PhoneStatusFrame is a wire contract");

// How long a phone's frames keep a badge conducting after the last one arrives.
// Longer than the ESP-NOW stale window: a phone that locks its screen or drops
// a connection should fade out, not cut off mid-beat.
#define PHONE_LINK_STALE_MS 1500

// A real conductor always wins. A phone only conducts when nothing else is on
// the air for this long -- otherwise two sources fight over one swarm.
#define PHONE_LINK_YIELD_MS 2000
