// ESP-NOW broadcast for the conductor.
//
// No router exists at camp, so nothing negotiates anything: the channel is a
// hardcoded constant that every badge must share, and the peer is the broadcast
// address. Switch it on and it works, which is the whole requirement.
#pragma once

#include <stdint.h>

#include "chorus_command.h"
#include "chorus_packet.h"
#include "conductor_config.h"

class ChorusRadio {
 public:
  // Throttles the CPU across the bring-up current spike, comes up at reduced TX
  // power, then restores full speed. Safe to call again after a failure -- the
  // conductor retries rather than treating a dead radio as fatal.
  bool begin();
  bool ready() const { return _ready; }
  // Fills in magic/seq/hop and sends. Returns false if the send was rejected.
  bool broadcast(uint8_t shader, float bass, float mid, float treble, float energy);

  // Address one badge, or the whole swarm with a {0,0,0} target. Commands are
  // rare and unacknowledged, so they go out CHORUS_CMD_REPEATS times: a single
  // lost frame would otherwise mean a button on a phone that silently did
  // nothing, which is worse than the airtime.
  bool command(uint8_t op, const uint8_t target[3], uint8_t arg0 = 0, uint8_t arg1 = 0,
               uint8_t arg2 = 0);

  uint16_t seq() const { return _seq; }
  uint32_t sent() const { return _sent; }
  uint32_t sendFailures() const { return _failures; }
  // Packets heard back from the swarm -- relayed copies of our own broadcasts.
  // A non-zero count is proof the mesh is alive without anyone walking the site.
  static uint32_t echoesHeard();
  static uint8_t lastEchoHop();

  // --- the roster ---------------------------------------------------------
  // Every badge beacons who it is and what it is showing (ChorusHello). The
  // leader keeps the last one heard from each, so a phone can be handed a list
  // of the swarm rather than having to know it in advance. Best-effort by
  // design: a missed beacon costs a badge two seconds of staleness, nothing
  // more, and a roll call brings everyone back at once.
  struct RosterEntry {
    uint8_t id[3];
    uint8_t shader;
    uint8_t flags;  // ChorusHelloFlag
    uint8_t fps;
    uint8_t crest;
    uint8_t hop;
    uint8_t rxPerSec;
    uint16_t uptimeS;
    uint32_t lastSeenMs;
  };
  static constexpr int ROSTER_MAX = 40;
  static int rosterCount();
  static const RosterEntry *rosterAt(int i);
  // Drop badges not heard from in this long, so a roster on a phone reflects
  // who is actually in the field rather than who was here an hour ago.
  static void rosterExpire(uint32_t olderThanMs);

 private:
  bool bringUp();
  bool _ready = false;
  uint16_t _seq = 0;
  uint32_t _sent = 0;
  uint32_t _failures = 0;
};
