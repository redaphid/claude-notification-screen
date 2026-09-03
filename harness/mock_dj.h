// The mock DJ: a deterministic stand-in for the conductor badge's audio
// analysis, so effects can be developed without a microphone, without
// hardware, and without a rave.
//
// It produces exactly what the real conductor will put on the wire (bass,
// mid, treble, energy) plus the two things the packet does NOT carry and each
// badge derives locally: `beat` (one frame per onset) and `beat_env` (the
// designed attack-decay envelope).
//
// The envelope is the whole point of the project's visual thesis, so it is
// modelled explicitly here rather than being faked with a smoothed signal:
//   attack  linear ramp to 1.0 over MOCK_DJ_ATTACK_MS
//   decay   exponential with MOCK_DJ_DECAY_MS time constant
// At 118 BPM the beat period is 508ms, so a 130ms time constant lands the
// envelope at ~0.02 just before the next hit: fully separated pulses.
#pragma once

#include <stdint.h>
#include "effect.h"

#define MOCK_DJ_ATTACK_MS 18.0f
#define MOCK_DJ_DECAY_MS 130.0f

typedef struct {
  float bpm;
  uint32_t t0_ms;      // start of the timeline; deliberately near the uint32
                       // wrap so every render exercises time_ms rollover
  double beat_period;  // ms
  long last_beat_index;
} MockDj;

void mock_dj_init(MockDj *dj, float bpm, uint32_t t0_ms);

// Fills `out` for absolute timeline position `elapsed_ms` (monotonic, starting
// at 0). Call once per frame with increasing values.
void mock_dj_frame(MockDj *dj, double elapsed_ms, EffectInput *out);
