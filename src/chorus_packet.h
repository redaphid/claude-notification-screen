// The contract every badge in the swarm speaks. FROZEN -- the same 24 bytes
// travel from Heltec bench tests through to the camp rave. Any change here has
// to be coordinated across every firmware stream at once.
#pragma once

#include <stdint.h>

static constexpr char CHORUS_MAGIC[4] = {'C', 'R', 'N', 'S'};
static constexpr uint8_t CHORUS_MAX_HOP = 3;

// features[] index names -- bass/mid/treble/energy, each 0..1.
enum ChorusFeature : uint8_t {
  FEAT_BASS = 0,
  FEAT_MID = 1,
  FEAT_TREBLE = 2,
  FEAT_ENERGY = 3,
  FEAT_COUNT = 4,
};

struct __attribute__((packed)) ChorusPacket {
  char magic[4];               // "CRNS"
  uint16_t seq;                // wraps; used for dedupe across relay paths
  uint8_t hop;                 // incremented on rebroadcast, dropped past CHORUS_MAX_HOP
  uint8_t shader;              // "everyone switch to 3"
  float features[FEAT_COUNT];  // bass, mid, treble, energy
};

static_assert(sizeof(ChorusPacket) == 24, "ChorusPacket is a wire contract; size must not drift");

static inline bool chorusPacketValid(const uint8_t *data, int len) {
  if (len != (int)sizeof(ChorusPacket)) return false;
  return data[0] == 'C' && data[1] == 'R' && data[2] == 'N' && data[3] == 'S';
}

// Sequence comparison that survives the uint16 wrap.
static inline bool chorusSeqNewer(uint16_t candidate, uint16_t known) {
  return (int16_t)(candidate - known) > 0;
}
