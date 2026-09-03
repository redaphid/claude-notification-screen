// Signal chain for the conductor: high-pass -> Hann window -> real FFT ->
// 4-band aggregation -> venue-adaptive normalisation -> onset detection ->
// asymmetric envelope shaping.
//
// Deliberately free of Arduino and ESP-IDF headers so the whole chain can be
// unit-tested on a laptop later without a board attached.
#pragma once

#include <stdint.h>

#include "conductor_config.h"

// --- Biquad ----------------------------------------------------------------
// Direct Form II transposed, float state. One instance is enough: everything
// upstream of the FFT is mono.
struct Biquad {
  float b0, b1, b2, a1, a2;
  float z1, z2;

  void reset();
  // Second-order high-pass (RBJ cookbook) with an explicit pole Q.
  void designHighPass(float sampleRate, float cutoffHz, float q);
  inline float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// Cascade of HPF_STAGES biquads forming one high-order Butterworth high-pass.
// This is the swarm's only defence against pulsing to the weather, so it is not
// a token one-pole.
struct HighPassCascade {
  Biquad stage[HPF_STAGES];
  void design(float sampleRate, float cutoffHz);
  void reset();
  inline float process(float x) {
    for (int i = 0; i < HPF_STAGES; i++) x = stage[i].process(x);
    return x;
  }
};

// --- Exponentially-weighted Welford ---------------------------------------
// Welford's incremental mean/variance, with the 1/n weight floored at 1/nCap so
// the estimator stops converging and starts tracking. Below nCap it behaves
// exactly like textbook Welford (correct during warm-up); above it, it is a
// first-order low-pass on both mean and variance with a time constant of nCap
// frames. This is what makes a packed dance floor and a quiet field both land
// in 0..1 rather than one of them pinning.
struct Welford {
  float mean;
  float var;
  uint32_t n;
  uint32_t nCap;

  void reset(uint32_t capFrames);
  void update(float x);
  float sigma() const;
  // (x - mean) / sigma, guarded against a degenerate sigma.
  float zscore(float x) const;
};

// --- Real FFT --------------------------------------------------------------
// N real samples via an N/2-point complex FFT plus the standard split step:
// half the butterflies of the naive "stuff zeros in the imaginary part"
// approach, which matters when this runs 62.5 times a second alongside a radio.
class RealFFT {
 public:
  void init();
  // in[FFT_SIZE] real samples (already windowed); out[FFT_BINS] magnitudes.
  void magnitude(const float *in, float *outMag);

 private:
  static constexpr int kHalf = FFT_SIZE / 2;
  float _re[kHalf];
  float _im[kHalf];
  float _twRe[kHalf / 2];  // twiddles for the half-length complex FFT
  float _twIm[kHalf / 2];
  float _splitRe[kHalf];  // e^{-2*pi*i*k/N} for the untangle step
  float _splitIm[kHalf];
  uint16_t _rev[kHalf];
  bool _ready = false;

  void complexFFT();
};

// --- Analyser output -------------------------------------------------------
struct AudioFeatures {
  // Normalised 0..1, asymmetric-smoothed, onset-shaped. These are the four
  // floats that go on the wire.
  float bass;
  float mid;
  float treble;
  float energy;

  // Diagnostics -- serial only, never transmitted.
  float rawDb[4];   // pre-normalisation log magnitudes
  float normRaw[4]; // post-normalisation, pre-smoothing
  float flux;
  float fluxThreshold;
  float gate;       // 1 = music is playing, 0 = the room has gone quiet
  bool onset;       // true only on the frame an onset fired
  float beatEnv;    // 1.0 at onset, decaying
  uint32_t onsetCount;
  uint32_t frameCount;
};

// --- Analyser --------------------------------------------------------------
class AudioAnalyzer {
 public:
  void init();
  // Feed exactly HOP_SIZE new mono samples (any scale; int16 range is fine).
  // Returns the features for the window ending at these samples.
  const AudioFeatures &pushHop(const float *hopSamples);

  const AudioFeatures &features() const { return _out; }

 private:
  RealFFT _fft;
  HighPassCascade _hpf;

  float _ring[FFT_SIZE];  // most recent FFT_SIZE filtered samples, linear
  float _window[FFT_SIZE];
  float _scratch[FFT_SIZE];
  float _mag[FFT_BINS];
  float _prevMag[FFT_BINS];

  int _bandLo[4];
  int _bandHi[4];
  int _fluxLo, _fluxHi;  // detection function band; not the same as the energy band

  Welford _norm[4];
  Welford _fluxStat;

  float _env[4];        // asymmetric-smoothed normalised features
  float _beatEnv;       // synthesised onset envelope
  float _gate;          // silence gate: 0 when the room is not playing music
  float _bandFluxShare[4];
  float _attackA, _releaseA, _beatDecayA;

  bool _armed;
  uint32_t _lastOnsetFrame;
  uint32_t _refractoryFrames;

  AudioFeatures _out;

  void computeBands(float *dbOut);
};
