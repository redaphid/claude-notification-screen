#include "dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ---------------------------------------------------------------------------
// Biquad
// ---------------------------------------------------------------------------
void Biquad::reset() { z1 = z2 = 0.0f; }

void Biquad::designHighPass(float sampleRate, float cutoffHz, float q) {
  // RBJ audio EQ cookbook, high-pass. Coefficients are worked out in double
  // because at 40 Hz / 16 kHz the cosine is within 1e-4 of 1 and float
  // cancellation there is visible in the passband.
  double w0 = 2.0 * M_PI * (double)cutoffHz / (double)sampleRate;
  double cw = cos(w0);
  double sw = sin(w0);
  double alpha = sw / (2.0 * (double)q);

  double a0 = 1.0 + alpha;
  double B0 = (1.0 + cw) / 2.0;
  double B1 = -(1.0 + cw);
  double B2 = (1.0 + cw) / 2.0;
  double A1 = -2.0 * cw;
  double A2 = 1.0 - alpha;

  b0 = (float)(B0 / a0);
  b1 = (float)(B1 / a0);
  b2 = (float)(B2 / a0);
  a1 = (float)(A1 / a0);
  a2 = (float)(A2 / a0);
  reset();
}

void HighPassCascade::reset() {
  for (int i = 0; i < HPF_STAGES; i++) stage[i].reset();
}

void HighPassCascade::design(float sampleRate, float cutoffHz) {
  static const float kQ[3] = {HPF_Q_STAGE_0, HPF_Q_STAGE_1, HPF_Q_STAGE_2};
  for (int i = 0; i < HPF_STAGES; i++)
    stage[i].designHighPass(sampleRate, cutoffHz, kQ[i % 3]);
}

// ---------------------------------------------------------------------------
// Welford
// ---------------------------------------------------------------------------
void Welford::reset(uint32_t capFrames) {
  mean = 0.0f;
  var = 0.0f;
  n = 0;
  nCap = capFrames < 2 ? 2 : capFrames;
}

void Welford::update(float x) {
  if (n < nCap) n++;
  float alpha = 1.0f / (float)n;  // floors at 1/nCap -> becomes a tracker
  float delta = x - mean;
  mean += alpha * delta;
  // Exponentially-weighted form of Welford's M2 update. With alpha = 1/n and n
  // uncapped this reproduces the population variance exactly; with alpha
  // floored it decays old evidence at the same rate as the mean.
  var = (1.0f - alpha) * (var + alpha * delta * delta);
}

float Welford::sigma() const {
  float s = var > 0.0f ? sqrtf(var) : 0.0f;
  return s < NORM_MIN_SIGMA ? NORM_MIN_SIGMA : s;
}

float Welford::zscore(float x) const { return (x - mean) / sigma(); }

// ---------------------------------------------------------------------------
// RealFFT
// ---------------------------------------------------------------------------
void RealFFT::init() {
  if (_ready) return;

  int bits = 0;
  while ((1 << bits) < kHalf) bits++;

  for (int i = 0; i < kHalf; i++) {
    unsigned r = 0;
    for (int b = 0; b < bits; b++)
      if (i & (1 << b)) r |= 1u << (bits - 1 - b);
    _rev[i] = (uint16_t)r;
  }

  for (int j = 0; j < kHalf / 2; j++) {
    double a = -2.0 * M_PI * (double)j / (double)kHalf;
    _twRe[j] = (float)cos(a);
    _twIm[j] = (float)sin(a);
  }

  for (int k = 0; k < kHalf; k++) {
    double a = -2.0 * M_PI * (double)k / (double)FFT_SIZE;
    _splitRe[k] = (float)cos(a);
    _splitIm[k] = (float)sin(a);
  }

  _ready = true;
}

void RealFFT::complexFFT() {
  // In-place bit-reversal permutation.
  for (int i = 0; i < kHalf; i++) {
    int j = _rev[i];
    if (j > i) {
      float tr = _re[i];
      _re[i] = _re[j];
      _re[j] = tr;
      float ti = _im[i];
      _im[i] = _im[j];
      _im[j] = ti;
    }
  }

  for (int len = 2; len <= kHalf; len <<= 1) {
    int half = len >> 1;
    int step = kHalf / len;  // stride into the twiddle table
    for (int i = 0; i < kHalf; i += len) {
      int tw = 0;
      for (int j = 0; j < half; j++, tw += step) {
        float wr = _twRe[tw];
        float wi = _twIm[tw];
        int a = i + j;
        int b = a + half;
        float xr = _re[b] * wr - _im[b] * wi;
        float xi = _re[b] * wi + _im[b] * wr;
        _re[b] = _re[a] - xr;
        _im[b] = _im[a] - xi;
        _re[a] += xr;
        _im[a] += xi;
      }
    }
  }
}

void RealFFT::magnitude(const float *in, float *outMag) {
  // Pack the N real samples into N/2 complex ones: even -> real, odd -> imag.
  for (int i = 0; i < kHalf; i++) {
    _re[i] = in[2 * i];
    _im[i] = in[2 * i + 1];
  }

  complexFFT();

  // Untangle. X[k] = Fe[k] + W_N^k * Fo[k], with
  //   Fe = (Z[k] + conj(Z[M-k])) / 2
  //   Fo = (Z[k] - conj(Z[M-k])) / (2i)
  float z0r = _re[0];
  float z0i = _im[0];
  outMag[0] = fabsf(z0r + z0i);            // DC
  outMag[kHalf] = fabsf(z0r - z0i);        // Nyquist

  for (int k = 1; k < kHalf; k++) {
    int m = kHalf - k;
    float ar = _re[k], ai = _im[k];
    float br = _re[m], bi = -_im[m];  // conj(Z[M-k])

    float fer = 0.5f * (ar + br);
    float fei = 0.5f * (ai + bi);
    // divide by 2i == multiply by -0.5i
    float dr = ar - br;
    float di = ai - bi;
    float forr = 0.5f * di;
    float foi = -0.5f * dr;

    float wr = _splitRe[k];
    float wi = _splitIm[k];
    float xr = fer + (wr * forr - wi * foi);
    float xi = fei + (wr * foi + wi * forr);
    outMag[k] = sqrtf(xr * xr + xi * xi);
  }
}

// ---------------------------------------------------------------------------
// AudioAnalyzer
// ---------------------------------------------------------------------------
static inline int hzToBin(float hz) {
  int b = (int)(hz * (float)FFT_SIZE / (float)AUDIO_SAMPLE_RATE + 0.5f);
  if (b < 0) b = 0;
  if (b > FFT_SIZE / 2) b = FFT_SIZE / 2;
  return b;
}

void AudioAnalyzer::init() {
  _fft.init();
  _hpf.design((float)AUDIO_SAMPLE_RATE, HPF_CUTOFF_HZ);

  memset(_ring, 0, sizeof(_ring));
  memset(_mag, 0, sizeof(_mag));
  memset(_prevMag, 0, sizeof(_prevMag));
  memset(&_out, 0, sizeof(_out));

  for (int i = 0; i < FFT_SIZE; i++)
    _window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(FFT_SIZE - 1)));

  const float lo[4] = {BAND_BASS_LO, BAND_MID_LO, BAND_TREBLE_LO, BAND_ENERGY_LO};
  const float hi[4] = {BAND_BASS_HI, BAND_MID_HI, BAND_TREBLE_HI, BAND_ENERGY_HI};
  for (int i = 0; i < 4; i++) {
    _bandLo[i] = hzToBin(lo[i]);
    _bandHi[i] = hzToBin(hi[i]);
    if (_bandLo[i] < 1) _bandLo[i] = 1;  // never let DC into a band
    if (_bandHi[i] <= _bandLo[i]) _bandHi[i] = _bandLo[i] + 1;
    _norm[i].reset((uint32_t)(NORM_WINDOW_S * FRAME_RATE_HZ));
    _env[i] = 0.0f;
    _bandFluxShare[i] = 0.0f;
  }

  _fluxLo = hzToBin(ONSET_FLUX_LO_HZ);
  _fluxHi = hzToBin(ONSET_FLUX_HI_HZ);
  if (_fluxLo < 1) _fluxLo = 1;
  if (_fluxHi <= _fluxLo) _fluxHi = _fluxLo + 1;

  _fluxStat.reset((uint32_t)(ONSET_STAT_WINDOW_S * FRAME_RATE_HZ));

  _attackA = 1.0f - expf(-FRAME_DT_S / (ENV_ATTACK_MS * 0.001f));
  _releaseA = 1.0f - expf(-FRAME_DT_S / (ENV_RELEASE_MS * 0.001f));
  _beatDecayA = expf(-FRAME_DT_S / (BEAT_ENV_DECAY_MS * 0.001f));

  _beatEnv = 0.0f;
  _gate = 0.0f;
  _armed = true;
  _lastOnsetFrame = 0;
  _refractoryFrames = (uint32_t)((ONSET_REFRACTORY_MS * 0.001f) * FRAME_RATE_HZ + 0.5f);
  if (_refractoryFrames < 1) _refractoryFrames = 1;
}

void AudioAnalyzer::computeBands(float *dbOut) {
  for (int i = 0; i < 4; i++) {
    float sum = 0.0f;
    for (int k = _bandLo[i]; k < _bandHi[i]; k++) sum += _mag[k];
    float mean = sum / (float)(_bandHi[i] - _bandLo[i]);
    // Log domain, because loudness is logarithmic and because it is the only
    // way a whisper-quiet field and a wall of PA both land in the same 0..1.
    // The +1 floor keeps digital silence from producing -inf.
    dbOut[i] = logf(1.0f + mean);
  }
}

const AudioFeatures &AudioAnalyzer::pushHop(const float *hopSamples) {
  // Slide the analysis window and append the new hop, high-passed.
  memmove(_ring, _ring + HOP_SIZE, (FFT_SIZE - HOP_SIZE) * sizeof(float));
  float *dst = _ring + (FFT_SIZE - HOP_SIZE);
  for (int i = 0; i < HOP_SIZE; i++) dst[i] = _hpf.process(hopSamples[i]);

  for (int i = 0; i < FFT_SIZE; i++) _scratch[i] = _ring[i] * _window[i];
  _fft.magnitude(_scratch, _mag);

  // --- spectral flux, in the log domain, half-wave rectified ---------------
  float flux = 0.0f;
  float bandFlux[4] = {0, 0, 0, 0};
  int lo = _fluxLo;
  int hi = _fluxHi;
  for (int k = lo; k < hi; k++) {
    float lm = logf(1.0f + _mag[k]);
    float d = lm - _prevMag[k];
    _prevMag[k] = lm;
    if (d > 0.0f) {
      flux += d;
      for (int b = 0; b < 3; b++)
        if (k >= _bandLo[b] && k < _bandHi[b]) bandFlux[b] += d;
    }
  }
  flux /= (float)(hi - lo);

  // --- adaptive threshold with hysteresis and a refractory period ----------
  float mu = _fluxStat.mean;
  float sd = _fluxStat.sigma();
  float thrHi = mu + ONSET_K_HI * sd;
  float thrLo = mu + ONSET_K_LO * sd;
  if (thrHi < ONSET_FLUX_FLOOR) thrHi = ONSET_FLUX_FLOOR;

  bool onset = false;
  bool refractoryOver = (_out.frameCount - _lastOnsetFrame) >= _refractoryFrames;
  if (_armed && flux > thrHi && refractoryOver && _fluxStat.n > NORM_WARMUP_FRAMES) {
    onset = true;
    _armed = false;
    _lastOnsetFrame = _out.frameCount;
  } else if (!_armed && flux < thrLo) {
    _armed = true;  // hysteresis: one sustained swell is one onset, not twelve
  }
  _fluxStat.update(flux);

  // --- band aggregation + venue-adaptive normalisation ---------------------
  float db[4];
  computeBands(db);

  float normRaw[4];
  for (int i = 0; i < 4; i++) {
    _norm[i].update(db[i]);
    if (_norm[i].n < NORM_WARMUP_FRAMES) {
      normRaw[i] = 0.0f;
    } else {
      float z = _norm[i].zscore(db[i]);
      normRaw[i] = clamp01((z - NORM_Z_LO) / (NORM_Z_HI - NORM_Z_LO));
    }
  }

  // --- asymmetric smoothing: fast attack, slow release ---------------------
  for (int i = 0; i < 4; i++) {
    float target = normRaw[i];
    float a = (target > _env[i]) ? _attackA : _releaseA;
    _env[i] += (target - _env[i]) * a;
  }

  // --- synthesised onset response ------------------------------------------
  // The packet has no triggers field, so the event is expressed by SHAPING the
  // feature envelopes rather than by flagging it. An onset slams the relevant
  // bands to full and then releases on a designed curve; a badge sees a sharp
  // rise followed by a smooth decay, which is exactly the animation it wants,
  // and never has to do its own smoothing.
  if (onset) {
    _beatEnv = 1.0f;
    float tot = bandFlux[0] + bandFlux[1] + bandFlux[2];
    if (tot > 1e-9f) {
      for (int b = 0; b < 3; b++) _bandFluxShare[b] = bandFlux[b] / tot;
    } else {
      for (int b = 0; b < 3; b++) _bandFluxShare[b] = 0.33f;
    }
    // Sharpen: a band that owns most of the transient goes all the way up.
    for (int b = 0; b < 3; b++) {
      float s = _bandFluxShare[b] * 1.8f;
      _bandFluxShare[b] = clamp01(s);
    }
    _bandFluxShare[3] = 1.0f;  // energy always carries the beat
  } else {
    _beatEnv *= _beatDecayA;
  }

  // --- silence gate ---------------------------------------------------------
  // The slow flux average says whether anything is actually being played. Slewed
  // with the same attack/release constants so it opens on the first bar and does
  // not chatter in a breakdown.
  float gateTarget = clamp01(_fluxStat.mean / SILENCE_GATE_FLUX);
  _gate += (gateTarget - _gate) * (gateTarget > _gate ? _attackA : _releaseA);

  float tx[4];
  for (int i = 0; i < 4; i++) {
    float shaped = _beatEnv * _bandFluxShare[i];
    float v = _env[i] > shaped ? _env[i] : shaped;
    _out.rawDb[i] = db[i];
    _out.normRaw[i] = normRaw[i];
    tx[i] = clamp01(v * _gate);
  }
  _out.bass = tx[0];
  _out.mid = tx[1];
  _out.treble = tx[2];
  _out.energy = tx[3];

  _out.flux = flux;
  _out.fluxThreshold = thrHi;
  _out.gate = _gate;
  _out.onset = onset;
  _out.beatEnv = _beatEnv;
  if (onset) _out.onsetCount++;
  _out.frameCount++;
  return _out;
}
