// Live parameters, the way paper-cranes does it.
//
// paper-cranes injects physical knobs into every shader as knob_1..knob_N
// uniforms, so a visual can be tuned while it is running and in front of you
// rather than edited, rebuilt and reloaded. Badges get the same idea over the
// air: eight knobs, broadcast to the swarm, applied live.
//
// This is deliberately NOT part of effect.h. That contract is frozen, and an
// effect is still a pure function of (features, time) -- knobs are ambient
// state it may read, exactly like mon_select() already is. effects.h has the
// precedent: "mon extras (not part of the frozen effect.h contract)".
//
// Slot meanings are kept consistent across effects on purpose. Somebody poking
// bytes into a generic BLE scanner has no labels in front of them, so knob 1
// should always be the same kind of thing:
//
//   1  reactivity -- how much the music is allowed to move this visual
//   2  scale      -- how big it is
//   3  speed      -- how fast it moves on its own
//   4  hue        -- colour rotation
//   5  glow       -- brightness / bloom
//   6..8          -- whatever the effect wants; read effect_knob_specs()
//
// Every knob is one byte on the wire and 0..1 to an effect. A byte because
// that is what a person can type into nRF Connect, and because 1/255 of a knob
// is already finer than anyone can see on a 240px disc.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNOB_COUNT 8

// What a knob means for one effect. A NULL name means the effect ignores that
// slot, which the UI shows as disabled rather than as a control that does
// nothing.
typedef struct {
  const char *name;
  uint8_t def;  // 0..255, applied when this effect is selected
} KnobSpec;

// KNOB_COUNT entries for the given effect index, never NULL -- an unknown
// index returns a table of unused slots rather than reading off the end.
const KnobSpec *effect_knob_specs(int effect_index);

// 0..1, what an effect reads. Out-of-range index returns 0.
float knob(int i);

// 0..255, what the wire and a BLE scanner carry.
uint8_t knob_raw(int i);
void knob_set(int i, uint8_t value);

// Put every knob back to what this effect declares as its default. Called when
// the effect changes: knob 6 means "crest" in mon and nothing in plasma, so
// carrying values across would leave a visual mysteriously wrong. Anyone who
// wants their tuning back re-sends it; anyone who does not gets the effect as
// its author intended.
void knobs_reset_for(int effect_index);

// Reset only when the effect has actually changed. Safe to call every frame,
// which is what callers do -- the badge's active shader is reassigned from
// every packet that arrives, so a plain reset there would stomp a knob a
// quarter of a second after somebody moved it.
void knobs_follow_effect(int effect_index);

#ifdef __cplusplus
}
#endif
