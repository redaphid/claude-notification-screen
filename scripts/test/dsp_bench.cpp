// Host-side bench for the conductor's analysis chain. There is no hardware to
// test on, so the parts that CAN be tested without a board are: the real FFT
// (against a naive DFT), the 40 Hz high-pass (against a rumble-only input), and
// the onset detector (against a synthesised loop with a known beat grid).
//
//   g++ -O2 -std=c++17 -I conductor -o /tmp/dsp_bench \
//       scripts/test/dsp_bench.cpp conductor/dsp.cpp && /tmp/dsp_bench
//
// This is a debug script, not a unit test: it prints and asserts loudly.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsp.h"

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) failures++;
}

// --- 1. FFT correctness ----------------------------------------------------
static void testFFT() {
  printf("\n[1] real FFT vs naive DFT\n");
  static float in[FFT_SIZE];
  static float mag[FFT_BINS];
  srand(12345);
  for (int i = 0; i < FFT_SIZE; i++) {
    in[i] = 1000.0f * sinf(2.0f * (float)M_PI * 37.0f * i / FFT_SIZE) +
            400.0f * cosf(2.0f * (float)M_PI * 211.0f * i / FFT_SIZE) +
            ((float)rand() / RAND_MAX - 0.5f) * 50.0f;
  }

  RealFFT fft;
  fft.init();
  fft.magnitude(in, mag);

  double worst = 0.0;
  double scale = 0.0;
  for (int k = 0; k <= FFT_SIZE / 2; k += 7) {  // sample the spectrum
    double re = 0, im = 0;
    for (int n = 0; n < FFT_SIZE; n++) {
      double a = -2.0 * M_PI * k * n / FFT_SIZE;
      re += in[n] * cos(a);
      im += in[n] * sin(a);
    }
    double ref = sqrt(re * re + im * im);
    double err = fabs(ref - mag[k]);
    if (ref > scale) scale = ref;
    if (err > worst) worst = err;
  }
  printf("       peak magnitude %.1f, worst absolute error %.4f (%.2e relative)\n", scale, worst,
         worst / scale);
  check(worst / scale < 1e-4, "real FFT matches the DFT to better than 1e-4 relative");

  // Peaks must land on the right bins.
  int p1 = 0, p2 = 0;
  for (int k = 1; k < FFT_SIZE / 2; k++) {
    if (mag[k] > mag[p1]) {
      p2 = p1;
      p1 = k;
    } else if (k != p1 && mag[k] > mag[p2]) {
      p2 = k;
    }
  }
  printf("       strongest bins: %d and %d (expected 37 and 211)\n", p1, p2);
  check((p1 == 37 && p2 == 211) || (p1 == 211 && p2 == 37), "tones land in the correct bins");
}

// --- 2. High-pass rejects sub-40 Hz ---------------------------------------
static void testHighPass() {
  printf("\n[2] 40 Hz high-pass vs wind rumble\n");
  HighPassCascade hp;

  auto gainAt = [&](float hz) {
    hp.design(AUDIO_SAMPLE_RATE, HPF_CUTOFF_HZ);
    double inPk = 0, outPk = 0;
    int n = AUDIO_SAMPLE_RATE * 2;
    for (int i = 0; i < n; i++) {
      float x = sinf(2.0f * (float)M_PI * hz * i / AUDIO_SAMPLE_RATE);
      float y = hp.process(x);
      if (i > n / 2) {  // let the filter settle
        if (fabsf(x) > inPk) inPk = fabsf(x);
        if (fabsf(y) > outPk) outPk = fabsf(y);
      }
    }
    return outPk / inPk;
  };

  double g10 = gainAt(10.0f), g25 = gainAt(25.0f), g40 = gainAt(40.0f);
  double g60 = gainAt(60.0f), g120 = gainAt(120.0f), g1k = gainAt(1000.0f);
  printf("       10Hz %.4f  25Hz %.4f  40Hz %.4f  60Hz %.4f  120Hz %.4f  1kHz %.4f\n", g10, g25,
         g40, g60, g120, g1k);
  check(g10 < 0.002, "10 Hz rumble attenuated by more than 50 dB");
  check(g25 < 0.10, "25 Hz handling noise attenuated by more than 20 dB");
  check(fabs(g40 - 0.7071) < 0.05, "40 Hz sits at the -3 dB corner, as Butterworth should");
  // The 1 kHz figure is peak-of-samples at 16 kHz -- 16 samples per cycle, so
  // the measurement itself can miss the true peak by up to 2%. 0.97 is the
  // measurement floor here, not the filter's.
  check(g60 > 0.95 && g120 > 0.97 && g1k > 0.97,
        "kick fundamentals and above pass essentially untouched");
}

// --- 3. Onset detection on a known beat grid ------------------------------
struct Gen {
  double t = 0.0;
  unsigned rng = 0x13579bdf;
  float noise() {
    rng = rng * 1664525u + 1013904223u;
    return ((float)(int)(rng >> 8) / 8388608.0f) - 1.0f;
  }
  // Same generator the fake-mic source uses, kept in sync by hand.
  void hop(float *out, double bpm, double venueGain, bool rumble) {
    const double dt = 1.0 / AUDIO_SAMPLE_RATE;
    const double beat = 60.0 / bpm;
    for (int i = 0; i < HOP_SIZE; i++) {
      double tt = t + i * dt;
      double phase = fmod(tt, beat);
      double halfPhase = fmod(tt, beat * 0.5);
      double kEnv = exp(-phase / 0.06);
      double kFreq = 55.0 + 45.0 * exp(-phase / 0.02);
      double kick = kEnv * sin(2.0 * M_PI * kFreq * phase);
      double hEnv = (phase > beat * 0.4) ? exp(-halfPhase / 0.025) : 0.0;
      double hat = hEnv * noise() * 0.5;
      double bassNote = 0.25 * sin(2.0 * M_PI * 82.0 * tt);
      double pad = 0.18 * sin(2.0 * M_PI * 440.0 * tt) * (0.6 + 0.4 * sin(2.0 * M_PI * 0.25 * tt));
      double rum = rumble ? (0.6 * sin(2.0 * M_PI * 11.0 * tt) + 0.3 * sin(2.0 * M_PI * 23.0 * tt))
                          : 0.0;
      out[i] = (float)((venueGain * (0.9 * kick + 0.5 * hat + bassNote + pad) + rum) * 9000.0);
    }
    t += HOP_SIZE * dt;
  }
};

static void testOnsets() {
  printf("\n[3] onset detection, 128 BPM for 20 s\n");
  AudioAnalyzer an;
  an.init();
  Gen g;
  float buf[HOP_SIZE];

  const double bpm = 128.0;
  int frames = (int)(20.0 * FRAME_RATE_HZ);
  int onsets = 0;
  double minGap = 1e9, maxTx = 0, minTxAfterWarm = 1e9;
  int lastOnsetFrame = -1000;
  double bassMax = 0, bassMin = 1;

  for (int i = 0; i < frames; i++) {
    g.hop(buf, bpm, 1.0, true);
    const AudioFeatures &f = an.pushHop(buf);
    if (f.onset) {
      if (lastOnsetFrame > -1000) {
        double gap = (i - lastOnsetFrame) / (double)FRAME_RATE_HZ;
        if (gap < minGap) minGap = gap;
      }
      lastOnsetFrame = i;
      onsets++;
    }
    if (i > 3 * FRAME_RATE_HZ) {
      if (f.energy > maxTx) maxTx = f.energy;
      if (f.energy < minTxAfterWarm) minTxAfterWarm = f.energy;
      if (f.bass > bassMax) bassMax = f.bass;
      if (f.bass < bassMin) bassMin = f.bass;
    }
  }
  double perSec = onsets / 20.0;
  printf("       %d onsets in 20 s (%.2f/s); beats are %.2f/s, offbeat hats double that\n", onsets,
         perSec, bpm / 60.0);
  printf("       shortest gap between onsets: %.0f ms (refractory is %d ms)\n", minGap * 1000.0,
         ONSET_REFRACTORY_MS);
  printf("       energy range %.2f..%.2f, bass range %.2f..%.2f\n", minTxAfterWarm, maxTx, bassMin,
         bassMax);
  check(minGap * 1000.0 >= ONSET_REFRACTORY_MS - 20, "refractory period is respected");
  check(perSec > 1.5 && perSec < 6.0, "onset rate is musically plausible for 128 BPM");
  check(maxTx > 0.9, "onsets drive the transmitted energy envelope to full");
  check(minTxAfterWarm < 0.6, "the envelope actually releases between hits");
}

// --- 4. Venue adaptation ---------------------------------------------------
static void testVenueAdaptation() {
  printf("\n[4] venue adaptation: quiet field vs packed floor\n");
  auto run = [](double gain) {
    AudioAnalyzer an;
    an.init();
    Gen g;
    float buf[HOP_SIZE];
    int frames = (int)(30.0 * FRAME_RATE_HZ);
    double sum = 0, rawSum = 0;
    int n = 0;
    for (int i = 0; i < frames; i++) {
      g.hop(buf, 128.0, gain, true);
      const AudioFeatures &f = an.pushHop(buf);
      if (i > 10 * FRAME_RATE_HZ) {
        sum += f.energy;
        rawSum += f.rawDb[3];
        n++;
      }
    }
    return std::pair<double, double>(sum / n, rawSum / n);
  };

  auto quiet = run(0.02);  // a field, 34 dB down
  auto loud = run(1.0);    // a wall of PA
  printf("       quiet field: raw %.2f -> mean transmitted energy %.3f\n", quiet.second,
         quiet.first);
  printf("       packed floor: raw %.2f -> mean transmitted energy %.3f\n", loud.second,
         loud.first);
  check(fabs(quiet.second - loud.second) > 2.0, "the two venues really are far apart in raw level");
  check(fabs(quiet.first - loud.first) < 0.20,
        "after normalisation both venues land in the same part of 0..1");
  check(quiet.first > 0.1 && quiet.first < 0.9, "the quiet venue neither floors nor pins");
  check(loud.first > 0.1 && loud.first < 0.9, "the loud venue neither floors nor pins");
}

// --- 5. Wind-only input must not drive the swarm --------------------------
static void testWindImmunity() {
  printf("\n[5] wind and handling noise only, no music\n");
  AudioAnalyzer an;
  an.init();
  float buf[HOP_SIZE];
  double t = 0;
  const double dt = 1.0 / AUDIO_SAMPLE_RATE;
  int frames = (int)(20.0 * FRAME_RATE_HZ);
  int onsets = 0;
  double bassMax = 0;
  double windFluxMax = 0;
  unsigned rng = 99;

  // Reference: what does the flux look like when actual music is playing?
  double musicFluxPeak = 0;
  {
    AudioAnalyzer ref;
    ref.init();
    Gen g;
    float rb[HOP_SIZE];
    for (int i = 0; i < (int)(10 * FRAME_RATE_HZ); i++) {
      g.hop(rb, 128.0, 1.0, true);
      const AudioFeatures &f = ref.pushHop(rb);
      if (i > 3 * FRAME_RATE_HZ && f.flux > musicFluxPeak) musicFluxPeak = f.flux;
    }
  }
  for (int i = 0; i < frames; i++) {
    for (int j = 0; j < HOP_SIZE; j++) {
      double tt = t + j * dt;
      rng = rng * 1664525u + 1013904223u;
      double gust = 0.5 + 0.5 * sin(2.0 * M_PI * 0.3 * tt);
      double s = gust * (1.0 * sin(2.0 * M_PI * 7.0 * tt) + 0.8 * sin(2.0 * M_PI * 14.0 * tt) +
                         0.5 * sin(2.0 * M_PI * 26.0 * tt));
      buf[j] = (float)(s * 12000.0);
    }
    t += HOP_SIZE * dt;
    const AudioFeatures &f = an.pushHop(buf);
    if (i > 5 * FRAME_RATE_HZ) {
      if (f.onset) onsets++;
      if (f.bass > bassMax) bassMax = f.bass;
      if (f.flux > windFluxMax) windFluxMax = f.flux;
    }
  }
  printf("       %d onsets and peak bass %.3f from 15 s of pure sub-40 Hz rumble\n", onsets,
         bassMax);
  printf("       flux peak %.4f (music peaks around %.4f), raw energy dB %.2f\n", windFluxMax,
         musicFluxPeak, an.features().rawDb[3]);
  check(onsets == 0, "wind fires no false onsets at all");
  check(bassMax < 0.10, "the silence gate keeps wind out of the transmitted bass");
}

int main() {
  printf("conductor dsp bench: fs=%d fft=%d hop=%d (%.1f fps)\n", AUDIO_SAMPLE_RATE, FFT_SIZE,
         HOP_SIZE, FRAME_RATE_HZ);
  testFFT();
  testHighPass();
  testOnsets();
  testVenueAdaptation();
  testWindImmunity();
  printf("\n%s (%d failing)\n", failures ? "FAILURES" : "all checks passed", failures);
  return failures ? 1 : 0;
}
