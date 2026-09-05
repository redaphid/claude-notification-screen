// A second conversation on the same air, so the first one never has to change.
//
// src/chorus_packet.h is frozen: 24 bytes, 30 times a second, "here is the
// music". This file adds two more frame types alongside it -- an address*ed*
// command going out to the swarm, and a beacon coming back -- each with its own
// magic. A badge running the old firmware sees a length and a magic it does not
// recognise and drops the frame in chorusPacketValid(); nothing regresses. That
// is the whole reason this is a separate contract rather than four spare bytes
// borrowed from the packet everyone already speaks.
//
// Identity is the last three bytes of the WiFi STA MAC. Three bytes, not six,
// because the frame has to stay small and because src/main.cpp already treats
// that tail as a badge's name (KnownBadge::macTail). A target of {0,0,0} means
// everyone -- no real MAC tail is all-zero often enough to matter, and the cost
// of the collision is one badge obeying a broadcast it would have obeyed a
// moment later anyway.
#pragma once

#include <stdint.h>
#include <string.h>

static constexpr char CHORUS_CMD_MAGIC[4] = {'C', 'R', 'N', 'C'};
static constexpr char CHORUS_HELLO_MAGIC[4] = {'C', 'R', 'N', 'H'};

// Commands and beacons are rarer than music, so they can afford another hop
// than the 3 CHORUS_MAX_HOP gives a feature packet -- but only just. Each extra
// hop multiplies the retransmissions a dense crowd makes.
static constexpr uint8_t CHORUS_CMD_MAX_HOP = 3;
static constexpr uint8_t CHORUS_HELLO_MAX_HOP = 2;

enum ChorusCmdOp : uint8_t {
  CMD_NONE = 0,
  // arg0 = effect index. arg1 = seconds to hold it, 0 = until released.
  // A pin outranks the conductor's shader byte and outranks the compile-time
  // BADGE_LOCK_EFFECT: the bag builds pick a badge's *default*, and a human
  // with a phone in their hand is a better authority than a build flag.
  CMD_SET_EFFECT = 1,
  // Drop the pin and follow the conductor again.
  CMD_RELEASE = 2,
  // arg0 = seconds. Wash the screen so you can find one badge in a crowd of
  // thirty, which is otherwise genuinely hard in the dark.
  CMD_IDENTIFY = 3,
  // arg0 = 0..255 backlight.
  CMD_BRIGHTNESS = 4,
  // Answer with a hello now instead of at the next beacon. Costs one frame per
  // badge; the reply is staggered by MAC so thirty badges do not collide.
  CMD_ROLL_CALL = 5,
  // arg0 = knob index 0..KNOB_COUNT-1, arg1 = value 0..255. Live parameters,
  // the paper-cranes way: tune the visual while it is running rather than
  // rebuilding it. Sent to one badge or to the whole swarm.
  CMD_SET_KNOB = 6,
  // Put every knob back to what the current effect declares. The way out of
  // having turned something to a place you cannot see your way back from.
  CMD_RESET_KNOBS = 7,
  // arg0 = mon variant index. Which family crest a badge wears. Persisted, so
  // it survives the power bank being swapped -- a crest is an identity, not a
  // setting, and a badge that forgets whose it is at 3am is no use.
  CMD_SET_CREST = 8,
};

struct __attribute__((packed)) ChorusCommand {
  char magic[4];      // "CRNC"
  uint16_t seq;       // wrap-safe dedupe, counted by the sender
  uint8_t hop;        // incremented on rebroadcast
  uint8_t op;         // ChorusCmdOp
  uint8_t target[3];  // MAC tail, or {0,0,0} for the whole swarm
  uint8_t arg0;
  uint8_t arg1;
  uint8_t arg2;
  uint8_t reserved[2];
};
static_assert(sizeof(ChorusCommand) == 16, "ChorusCommand is a wire contract");

// Flags in ChorusHello::flags.
enum ChorusHelloFlag : uint8_t {
  HELLO_PINNED = 1 << 0,      // obeying a CMD_SET_EFFECT, not the conductor
  HELLO_HEARING = 1 << 1,     // a conductor is on the air right now
  HELLO_CONDUCTING = 1 << 2,  // this badge is itself the conductor
  HELLO_IDENTIFYING = 1 << 3,
};

struct __attribute__((packed)) ChorusHello {
  char magic[4];   // "CRNH"
  uint8_t id[3];   // this badge's MAC tail
  uint8_t seq;     // per-badge, wraps at 256; only used to dedupe relays
  uint8_t shader;  // what it is actually showing
  uint8_t flags;   // ChorusHelloFlag
  uint8_t fps;     // capped at 255
  uint8_t crest;   // mon variant index, so the roster can say "kiku"
  uint16_t uptimeS;
  uint8_t hop;      // how far this beacon travelled to be heard
  uint8_t rxPerSec;  // feature packets/s this badge is receiving
};
static_assert(sizeof(ChorusHello) == 16, "ChorusHello is a wire contract");

static inline bool chorusCommandValid(const uint8_t *data, int len) {
  if (len != (int)sizeof(ChorusCommand)) return false;
  return data[0] == 'C' && data[1] == 'R' && data[2] == 'N' && data[3] == 'C';
}

static inline bool chorusHelloValid(const uint8_t *data, int len) {
  if (len != (int)sizeof(ChorusHello)) return false;
  return data[0] == 'C' && data[1] == 'R' && data[2] == 'N' && data[3] == 'H';
}

static inline bool chorusIdEq(const uint8_t a[3], const uint8_t b[3]) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static inline bool chorusIdIsBroadcast(const uint8_t id[3]) {
  return id[0] == 0 && id[1] == 0 && id[2] == 0;
}

// The MAC tail as the rest of the firmware already spells it: 0xAABBCC.
static inline uint32_t chorusIdToTail(const uint8_t id[3]) {
  return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

static inline void chorusTailToId(uint32_t tail, uint8_t id[3]) {
  id[0] = (uint8_t)(tail >> 16);
  id[1] = (uint8_t)(tail >> 8);
  id[2] = (uint8_t)tail;
}
