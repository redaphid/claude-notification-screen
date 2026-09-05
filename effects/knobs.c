#include "knobs.h"

#include <stddef.h>

#include "effects.h"

static uint8_t s_knob[KNOB_COUNT];
// -1 so the first call always loads defaults, including for effect 0.
static int s_current = -1;

// Keep in step with effects_all[] in effects.c -- same order, same length.
// Append only, for the same reason the effect registry is: these tables are
// indexed by the shader byte that travels between boards.
//
// A slot left {0, 0} is unused by that effect.
static const KnobSpec s_specs[][KNOB_COUNT] = {
    // plasma
    {{"reactivity", 160}, {"scale", 128}, {"speed", 128}, {"hue", 0},
     {"glow", 160}, {NULL, 0}, {NULL, 0}, {NULL, 0}},
    // tunnel -- no scale slot: the tunnel's geometry is a baked LUT, so there
    // is nothing to scale without rebuilding it per frame.
    {{"reactivity", 160}, {NULL, 0}, {"speed", 128}, {"hue", 0},
     {"glow", 160}, {NULL, 0}, {NULL, 0}, {NULL, 0}},
    // iris -- same, and its brightness is already the shock-ring envelope.
    {{"reactivity", 160}, {NULL, 0}, {"speed", 128}, {"hue", 0},
     {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}},
    // mon -- the crest in its own colours
    {{"reactivity", 90}, {"size", 128}, {"spin", 128}, {"hue", 0},
     {"glow", 160}, {"kick", 128}, {NULL, 0}, {NULL, 0}},
    // chroma -- the crest as a ChromaDepth height map
    //
    // reactivity defaults low here on purpose. The paper-cranes work on this
    // same artwork concluded that loudness must not move the crest's geometry
    // or it stops being nameable, and chroma was the effect doing exactly that
    // (58 to 134px of radius across energy, bass and the beat envelope). The
    // knob does not settle that argument, it just puts it in the wearer's
    // hands: 0 holds the crest perfectly still, 255 is the old behaviour.
    {{"reactivity", 70}, {"size", 128}, {"spin", 128}, {"hue", 0},
     {"depth", 160}, {"kick", 128}, {NULL, 0}, {NULL, 0}},
};

static const KnobSpec s_none[KNOB_COUNT] = {{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0},
                                            {NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}};

const KnobSpec *effect_knob_specs(int effect_index) {
  const int n = (int)(sizeof(s_specs) / sizeof(s_specs[0]));
  if (effect_index < 0 || effect_index >= n) return s_none;
  return s_specs[effect_index];
}

float knob(int i) {
  if (i < 0 || i >= KNOB_COUNT) return 0.0f;
  return (float)s_knob[i] * (1.0f / 255.0f);
}

uint8_t knob_raw(int i) {
  if (i < 0 || i >= KNOB_COUNT) return 0;
  return s_knob[i];
}

void knob_set(int i, uint8_t value) {
  if (i < 0 || i >= KNOB_COUNT) return;
  s_knob[i] = value;
}

void knobs_reset_for(int effect_index) {
  const KnobSpec *spec = effect_knob_specs(effect_index);
  for (int i = 0; i < KNOB_COUNT; i++) s_knob[i] = spec[i].def;
  s_current = effect_index;
}

void knobs_follow_effect(int effect_index) {
  if (effect_index == s_current) return;
  knobs_reset_for(effect_index);
}
