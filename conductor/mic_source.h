// The one place the conductor touches hardware. Two implementations behind one
// interface:
//
//   real  -- I2S capture from the onboard mic of the ESP32-S3-Touch-LCD-1.46
//   fake  -- a synthesised 128 BPM loop, selected with -DCONDUCTOR_FAKE_MIC
//
// The fake source exists so the entire chain (filter, FFT, normalisation, onset
// detection, radio) can be exercised on any ESP32-S3 -- a Heltec V3 off the
// shelf, say -- long before the 1.46 comes out of its box. It generates into the
// same buffer, at the same rate, in the same units.
#pragma once

#include <stdint.h>

#include "conductor_config.h"

class MicSource {
 public:
  // Returns false if the hardware refused to come up; the caller should say so
  // loudly on serial rather than silently broadcasting silence.
  bool begin();

  // Blocks until HOP_SIZE mono samples are available and writes them to `out`
  // in roughly int16 units. Returns false on a read error / timeout.
  bool readHop(float *out);

  const char *sourceName() const;
  int selectedChannel() const { return _channel; }

 private:
  int _channel = 0;  // 0 = left slot, 1 = right slot

#ifndef CONDUCTOR_FAKE_MIC
  int16_t _raw[HOP_SIZE * 2];  // interleaved stereo frames
  void resolveChannel();
#else
  double _t = 0.0;       // seconds since boot, in generated time
  uint32_t _nextDueMs = 0;
  uint32_t _rngState = 0x13579bdfu;
  float noise();
#endif
};
