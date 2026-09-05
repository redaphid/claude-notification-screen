// The phone-to-leader BLE contract.
//
// The leader already knows everything a control surface needs: which effect the
// swarm is on, what the whole list of effects is called, and -- since the
// roster beacons -- which badges are in the field and what each is showing. It
// also already has the only radio that can reach them. So the phone does not
// talk to badges at all: it talks to the leader, and the leader speaks ESP-NOW.
//
// That is the whole reason this is a leader service rather than a badge one. A
// phone with Web Bluetooth can hold one or two connections; a swarm is thirty
// badges. One hop through the leader turns an impossible fan-out into a single
// connection, and it reuses the mesh relay that already exists to reach badges
// the phone could never see.
//
// This header is the contract between the firmware and web/control.html. Keep
// the two in step; the page reads these same UUIDs and this same byte layout.
#pragma once

#include <stdint.h>

// Distinct from the badge's phone-link service (c8a0f1..) on purpose: a phone
// scanning in a field will see both, and they are not interchangeable. The
// badge service makes a phone into a microphone; this one makes it a console.
#define LEADER_LINK_SERVICE_UUID "c8a0f200-0451-4000-b000-63726e730001"

// Phone -> leader. One command frame per write. Write-with-response, unlike the
// badge's feature characteristic: a command happens once and the person who
// pressed the button deserves to know it landed.
#define LEADER_LINK_CONTROL_UUID "c8a0f201-0451-4000-b000-63726e730001"
// Leader -> phone. Notify ~1Hz plus on change: what the swarm is doing.
#define LEADER_LINK_STATE_UUID "c8a0f202-0451-4000-b000-63726e730001"
// Leader -> phone. The roster, as a read; notified when it changes size.
#define LEADER_LINK_ROSTER_UUID "c8a0f203-0451-4000-b000-63726e730001"
// Leader -> phone. The effect names, newline separated, as a plain read. The
// page must not hardcode the registry: effects are append-only and a page
// shipped before an effect existed should still label it correctly.
#define LEADER_LINK_NAMES_UUID "c8a0f204-0451-4000-b000-63726e730001"

// --- pokeable attributes ---------------------------------------------------
// Everything below is one byte, readable and writable on its own
// characteristic, each carrying a 0x2901 Characteristic User Description. That
// is what makes a generic BLE scanner -- nRF Connect, LightBlue -- into a
// usable control surface: the app shows a name and a value and lets you type a
// new byte, with no page and no app of ours involved. The binary command
// characteristic above is for software; these are for fingers.
//
// Effect index, 0..effectCount-1. Writing it is the same as typing an effect
// name on the leader's serial console.
#define LEADER_LINK_EFFECT_UUID "c8a0f205-0451-4000-b000-63726e730001"
// Which family crest the whole swarm wears, 0..crestCount-1.
#define LEADER_LINK_CREST_UUID "c8a0f206-0451-4000-b000-63726e730001"
// Knobs 1..8, contiguous so a scanner lists them in order. Live parameters,
// broadcast to every badge as they are turned. See effects/knobs.h.
#define LEADER_LINK_KNOB_BASE_UUID "c8a0f21%d-0451-4000-b000-63726e730001"
// Newline-separated, in this order: the current effect's knob labels (always
// KNOB_COUNT lines, blank where the effect ignores that slot), a "--" line,
// then the crest names. Read it to label a UI; the page must not hardcode
// either list.
#define LEADER_LINK_LABELS_UUID "c8a0f207-0451-4000-b000-63726e730001"

// Advertised name. "Chorus Leader" rather than "Chorus-XXXX" so that in a field
// with badges advertising too, the one you want is the one you can read.
#define LEADER_LINK_NAME "Chorus Leader"

enum LeaderLinkOp : uint8_t {
  // arg0 = effect index. What the whole swarm follows: this is the shader byte
  // in every outgoing ChorusPacket, the same thing the serial console sets.
  LEADER_OP_SET_EFFECT = 1,
  LEADER_OP_NEXT = 2,
  LEADER_OP_PREV = 3,
  // arg0..arg1 = little-endian seconds. 0 holds the current effect.
  LEADER_OP_CYCLE_SECONDS = 4,
  // The rest address badges, and are forwarded verbatim as a ChorusCommand.
  // target = {0,0,0} is the whole swarm.
  LEADER_OP_BADGE_EFFECT = 16,
  LEADER_OP_BADGE_RELEASE = 17,
  LEADER_OP_BADGE_IDENTIFY = 18,
  LEADER_OP_BADGE_BRIGHTNESS = 19,
  LEADER_OP_ROLL_CALL = 20,
  // arg0 = knob index 0..KNOB_COUNT-1, arg1 = value. Applied to the leader's
  // own panel and broadcast to every badge, because the point of a knob is
  // that the swarm changes together.
  LEADER_OP_SET_KNOB = 21,
  LEADER_OP_RESET_KNOBS = 22,
  // arg0 = mon variant. target selects one badge or all of them.
  LEADER_OP_SET_CREST = 23,
};

struct __attribute__((packed)) LeaderControlFrame {
  uint8_t op;         // LeaderLinkOp
  uint8_t target[3];  // badge MAC tail; {0,0,0} = every badge
  uint8_t arg0;
  uint8_t arg1;
  uint8_t arg2;
  uint8_t reserved;
};
static_assert(sizeof(LeaderControlFrame) == 8, "LeaderControlFrame is a wire contract");

struct __attribute__((packed)) LeaderStateFrame {
  uint8_t shader;       // what the leader is broadcasting
  uint8_t effectCount;  // so the page can bound its own indices
  uint8_t badges;       // roster size
  uint8_t hearing;      // 1 if the mic gate is open -- is there music?
  uint16_t cycleS;      // auto-advance period, 0 = holding
  uint16_t txPerSec;    // packets/s the leader is putting on the air
  uint16_t uptimeS;
  uint8_t bass;  // 0..255, so the page can show a level and prove it is live
  uint8_t energy;
};
static_assert(sizeof(LeaderStateFrame) == 12, "LeaderStateFrame is a wire contract");

// One badge, as the roster characteristic reports it. The read returns a count
// byte followed by this repeated -- 8 bytes each, so twenty badges fit in a
// 161-byte read, inside the 244-byte ATT MTU a phone negotiates in practice.
struct __attribute__((packed)) LeaderRosterEntry {
  uint8_t id[3];
  uint8_t shader;
  uint8_t flags;  // ChorusHelloFlag: pinned / hearing / conducting / identifying
  uint8_t fps;
  uint8_t crest;
  uint8_t ageS;  // seconds since its last beacon, capped at 255
};
static_assert(sizeof(LeaderRosterEntry) == 8, "LeaderRosterEntry is a wire contract");

#ifdef LEADER_BLE

// Brings up NimBLE and starts advertising. Returns false if the controller
// refused, which must not be fatal: a leader with no phone attached is still a
// leader, and the serial console still works.
bool leaderLinkBegin();
bool leaderLinkConnected();
// Called once a second from the main loop with the numbers for the state frame.
void leaderLinkPublish(uint8_t shader, uint8_t effectCount, uint8_t badges, bool hearing,
                       uint16_t cycleS, uint16_t txPerSec, uint16_t uptimeS, uint8_t bass,
                       uint8_t energy);
// A phone's write is queued, not applied where it lands: the NimBLE host task
// is not a place to call esp_now_send() or walk the effect registry. The main
// loop drains this. Returns false when there is nothing waiting.
bool leaderLinkTakeCommand(LeaderControlFrame *out);
// Push the current knob values and this effect's labels to anything listening.
void leaderLinkPublishKnobs(uint8_t shader);

#else

// The leader builds without BLE by default -- see the note in platformio.ini on
// what the radio costs. These stubs keep conductor_main.cpp free of #ifdefs.
static inline bool leaderLinkBegin() { return false; }
static inline bool leaderLinkConnected() { return false; }
static inline void leaderLinkPublish(uint8_t, uint8_t, uint8_t, bool, uint16_t, uint16_t, uint16_t,
                                     uint8_t, uint8_t) {}
static inline bool leaderLinkTakeCommand(LeaderControlFrame *) { return false; }
static inline void leaderLinkPublishKnobs(uint8_t) {}

#endif
