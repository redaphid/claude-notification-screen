#include "mock_dj.h"

#include <math.h>

#define TAU 6.283185307179586

static float clamp01(double v) { return (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)); }

void mock_dj_init(MockDj *dj, float bpm, uint32_t t0_ms) {
  dj->bpm = bpm;
  dj->t0_ms = t0_ms;
  dj->beat_period = 60000.0 / (double)bpm;
  dj->last_beat_index = -1;
}

void mock_dj_frame(MockDj *dj, double elapsed_ms, EffectInput *out) {
  const double period = dj->beat_period;
  const double bar = period * 4.0;
  const double phrase = bar * 4.0;

  // ---- beat detection: fire on the frame that crosses a quarter note ------
  const long beat_index = (long)floor(elapsed_ms / period);
  const int fired = (beat_index != dj->last_beat_index);
  dj->last_beat_index = beat_index;

  const double since_beat = elapsed_ms - (double)beat_index * period;

  // ---- the designed envelope ---------------------------------------------
  double env;
  if (since_beat < MOCK_DJ_ATTACK_MS) {
    env = since_beat / MOCK_DJ_ATTACK_MS;
  } else {
    env = exp(-(since_beat - MOCK_DJ_ATTACK_MS) / MOCK_DJ_DECAY_MS);
  }

  // ---- band energies -----------------------------------------------------
  // Smooth LFOs on musical subdivisions, the way a real spectral envelope
  // moves once it has been smoothed by the conductor. No per-frame jitter:
  // if the effects look good here and bad on hardware, the smoothing on the
  // conductor is the thing that is wrong, not the effect.
  const double kick = pow(env, 1.6);  // the kick drum energy in the bass band
  const double bass = 0.30 + 0.22 * sin(TAU * elapsed_ms / phrase) + 0.42 * kick;

  // mid: a 2-bar pad swell with an eighth-note stab riding on it
  const double stab = 0.5 + 0.5 * sin(TAU * elapsed_ms / (period * 0.5));
  const double mid = 0.26 + 0.30 * (0.5 + 0.5 * sin(TAU * elapsed_ms / (bar * 2.0))) +
                     0.22 * stab * stab;

  // treble: hats on the offbeat, plus a slow filter sweep across the phrase
  const double hat = 0.5 + 0.5 * sin(TAU * (elapsed_ms / (period * 0.5) + 0.5));
  const double sweep = 0.5 + 0.5 * sin(TAU * elapsed_ms / (phrase * 2.0));
  const double treble = 0.14 + 0.46 * pow(hat, 3.0) * (0.45 + 0.55 * sweep);

  const double energy = 0.45 * bass + 0.35 * mid + 0.20 * treble + 0.18 * env;

  out->bass = clamp01(bass);
  out->mid = clamp01(mid);
  out->treble = clamp01(treble);
  out->energy = clamp01(energy);
  out->beat = (uint8_t)(fired ? 1 : 0);
  out->beat_env = clamp01(env);
  out->time_ms = (uint32_t)(dj->t0_ms + (uint32_t)(uint64_t)elapsed_ms);
}
