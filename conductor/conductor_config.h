// Conductor firmware configuration -- every number worth arguing about lives here.
//
// Target: Waveshare ESP32-S3-Touch-LCD-1.46 (onboard MSM-series I2S mic + PCM5101
// speaker). This is the ONE badge in the swarm with a microphone. Every other
// badge is deaf and renders what this board says.
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// PIN BLOCK -- CORRECT THESE FIRST AT BRING-UP IF THE MIC IS SILENT
// ---------------------------------------------------------------------------
// Source: Waveshare's own Arduino demo for this exact board,
//   waveshareteam/ESP32-S3-Touch-LCD-1.46
//   example/Arduino-3.1.1/examples/LVGL_Arduino/MIC_MSM.h
// which reads verbatim:
//   #define I2S_PIN_BCK   15
//   #define I2S_PIN_WS    2
//   #define I2S_PIN_DOUT  -1
//   #define I2S_PIN_DIN   39
// cross-checked against the pin tables on both wiki pages
// (https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.46B and
//  https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.46), which list
//   MIC_WS = GPIO2, MIC_SCK = GPIO15, MIC_SD = GPIO39.
// Two independent sources agree, so confidence here is high.
//
// That same demo calls i2s.begin(I2S_MODE_STD, 16000, 16BIT, SLOT_MODE_STEREO):
// this is a STANDARD I2S mic (Philips format), NOT a PDM mic. If it comes up
// silent, do not "fix" it by switching to PDM mode -- check the channel first.
#define MIC_PIN_BCLK 15  // MIC_SCK  -- high confidence
#define MIC_PIN_WS 2     // MIC_WS   -- high confidence
#define MIC_PIN_DIN 39   // MIC_SD   -- high confidence
#define MIC_PIN_MCLK -1  // not routed; MSM-series mics self-clock from BCLK

// A single I2S mic sits on one half of the stereo frame, chosen by an L/R strap
// pin the wiki does not document. Rather than guess: capture both slots and pick
// whichever actually carries signal.
//   0 = force left, 1 = force right, 2 = auto-detect at boot (default)
#ifndef MIC_CHANNEL_SELECT
#define MIC_CHANNEL_SELECT 2
#endif
#define MIC_AUTODETECT_MS 800  // how long auto-detect listens before deciding
// ---------------------------------------------------------------------------
// END PIN BLOCK
// ---------------------------------------------------------------------------

// --- Audio front end -------------------------------------------------------
// 16 kHz matches Waveshare's own configuration for this mic and gives an 8 kHz
// Nyquist, which is more treble than a dance floor ever needs.
#define AUDIO_SAMPLE_RATE 16000

// 1024-point analysis window = 64 ms. Chosen for BASS resolution: at this size a
// bin is 15.6 Hz, so the 40..160 Hz bass band gets 8 bins where a 512-point
// window would give 4. Kick fundamentals are the whole point of this rig.
#define FFT_SIZE 1024
#define FFT_BINS (FFT_SIZE / 2 + 1)

// 256-sample hop = 16 ms = 75% overlap. The hop, not the window, sets onset
// timing resolution, so it is deliberately much shorter than the window.
#define HOP_SIZE 256
#define FRAME_RATE_HZ ((float)AUDIO_SAMPLE_RATE / (float)HOP_SIZE)  // 62.5 Hz
#define FRAME_DT_S ((float)HOP_SIZE / (float)AUDIO_SAMPLE_RATE)     // 0.016 s

// --- High-pass filter ------------------------------------------------------
// The camp is a pine forest in September. Wind rumble and the handling noise of
// a badge on a lanyard both land at 5..30 Hz, squarely inside what a naive bass
// band integrates, and would make the entire swarm breathe with the weather.
//
// SIXTH-order Butterworth, not second. A 2-pole filter at 40 Hz leaves 25 Hz
// only 9 dB down, and the host bench (scripts/test/dsp_bench.cpp) showed that is
// not nearly enough: 15 s of pure sub-40 Hz gusting produced 53 false onsets and
// drove the bass feature to 0.67. Six poles put 25 Hz 24 dB down while leaving
// 60 Hz at 0.996, so kick fundamentals are untouched. Three cascaded biquads at
// the Butterworth pole Q values; the cost is three multiply-adds per sample.
#define HPF_CUTOFF_HZ 40.0f
#define HPF_STAGES 3
// Pole Q values for a 6th-order Butterworth. Cascading three Q=0.707 sections
// would NOT be Butterworth -- it would be 9 dB down at the corner instead of 3.
#define HPF_Q_STAGE_0 0.51763809f
#define HPF_Q_STAGE_1 0.70710678f
#define HPF_Q_STAGE_2 1.93185165f

// --- Band edges (Hz) -------------------------------------------------------
#define BAND_BASS_LO 40.0f
#define BAND_BASS_HI 160.0f
#define BAND_MID_LO 160.0f
#define BAND_MID_HI 2000.0f
#define BAND_TREBLE_LO 2000.0f
#define BAND_TREBLE_HI 7000.0f
#define BAND_ENERGY_LO 40.0f
#define BAND_ENERGY_HI 7000.0f

// --- Venue-adaptive normalisation -----------------------------------------
// Exponentially-weighted Welford over ~20 s of frames. The cap is what makes it
// adapt: a plain Welford accumulator has infinite memory and at 2 a.m. would
// still be normalising against soundcheck.
#define NORM_WINDOW_S 20.0f
#define NORM_WARMUP_FRAMES 24
// z-score -> 0..1 mapping. z = -1.5 maps to 0.0 and z = +2.0 maps to 1.0, so a
// loud passage sits near 0.7 and leaves headroom for a genuine peak.
#define NORM_Z_LO (-1.5f)
#define NORM_Z_HI (2.0f)
#define NORM_MIN_SIGMA 1e-3f

// --- Onset detection -------------------------------------------------------
// Spectral flux against an adaptive threshold, with hysteresis and a refractory
// period. This is the load-bearing idea of the project: the ~1 s lag in the
// browser version was never transport, it was heavy smoothing used to hide
// frame-to-frame shudder, because one signal was doing two incompatible jobs --
// measurement (wants fidelity) and animation drive (wants continuity). Split
// them: measure sharply, detect the event, then SYNTHESISE the smooth response.
//
// The detection function starts at 80 Hz, an octave above the high-pass corner,
// while the transmitted bass band still starts at 40 Hz. This is deliberate: a
// 1024-point Hann window has a four-bin (62 Hz) main lobe, so a 26 Hz gust smears
// its energy right up to bin 3 no matter how good the filter is. Bin 5 is outside
// that main lobe, and reaching it costs a gust the filter's 24 dB plus another
// 31 dB of Hann sidelobe. Kicks are unaffected -- a kick's onset is marked by its
// harmonics and beater click, not by its 55 Hz fundamental, which arrives late
// and slowly anyway.
#define ONSET_FLUX_LO_HZ 80.0f
#define ONSET_FLUX_HI_HZ 7000.0f
#define ONSET_STAT_WINDOW_S 0.8f  // adaptive threshold memory
#define ONSET_K_HI 1.9f           // fire above mean + K_HI * sigma
#define ONSET_K_LO 0.9f           // re-arm only below mean + K_LO * sigma
#define ONSET_REFRACTORY_MS 100
// Absolute floor under the adaptive threshold. An adaptive threshold alone is
// not enough: with no music playing it happily adapts DOWN to the noise floor
// and starts calling gusts beats. Measured on the host bench, 15 s of worst-case
// near-full-scale sub-40 Hz gusting peaks at flux 0.056, while the 128 BPM loop
// peaks at 5.84 -- two orders of magnitude apart, so this floor sits comfortably
// between them with margin on both sides.
#define ONSET_FLUX_FLOOR 0.25f

// Silence gate. Venue-adaptive normalisation has one unavoidable failure mode:
// in a silent room it will happily z-score the noise floor up to full scale, and
// the swarm ends up pulsing to whatever is left -- which at camp means wind.
// Flux separates "music is playing" from "nothing is playing" by 100x, so the
// slow flux average doubles as an activity detector, and the transmitted
// features are scaled by it. Music stops, swarm goes calm.
#define SILENCE_GATE_FLUX ONSET_FLUX_FLOOR

// --- Envelope shaping ------------------------------------------------------
// Asymmetric smoothing on the transmitted features: near-instant attack so a hit
// lands on the beat, slow release so the swarm glides instead of stuttering.
#define ENV_ATTACK_MS 8.0f
#define ENV_RELEASE_MS 180.0f
// The synthesised onset envelope mixed over the smoothed features. See the long
// comment in conductor_main.cpp for why this exists instead of a triggers field.
#define BEAT_ENV_DECAY_MS 140.0f

// --- Radio -----------------------------------------------------------------
// Fixed channel: no router exists at camp, so nothing negotiates. Every badge
// hardcodes this same number or the swarm never hears the conductor.
#define CHORUS_WIFI_CHANNEL 1

// Brownout mitigation. Bringing up the radio is the largest current spike this
// board ever draws, and on a marginal supply it collapses the 3V3 rail and takes
// the USB bridge down with the ESP32 -- which from the host looks exactly like a
// hang inside WiFi.mode(). The coordinator hit this on real badge hardware.
// Drop the core to 80 MHz across bring-up, come up at reduced TX power, then
// restore full speed. The conductor needs this more than the badges do: it is
// the only node transmitting continuously.
#define RADIO_BRINGUP_CPU_MHZ 80
// esp_wifi_set_max_tx_power units are 0.25 dBm. 52 = 13 dBm, roughly half the
// default 20 dBm in power terms and still far more than enough for a camp-sized
// site given CHORUS_MAX_HOP relaying. Raise it only with a meter on the rail.
#define RADIO_TX_POWER_QDBM 52
// If the radio does not come up, keep analysing and retry. A conductor that
// bricks itself over a failed antenna stage is worse than one that is briefly
// mute -- it still has a mic and a screen, and the rail may simply need a moment.
#define RADIO_RETRY_MS 5000
// Cadence is expressed in ANALYSIS HOPS, not milliseconds, and that is
// deliberate. The send decision is only ever evaluated once per hop, so a
// millisecond interval silently rounds up to the next whole hop: 33ms against a
// 16.1ms hop is never satisfied on the second hop (32.2ms, just short) and
// always waits for the third, giving 48.3ms -- 20.7Hz, not the 30Hz intended.
//
// That was not caught here for a long time because the conductor's own counters
// looked fine; it took four badges on another bench independently reporting
// ~15 packets/sec to expose it. Counting hops makes the cadence exact and
// removes the whole class of bug.
//
//   1 hop  = 62.5 Hz   2 hops = 31.25 Hz   3 hops = 20.8 Hz
#define PACKET_EVERY_N_HOPS 2
// An onset may jump the queue rather than wait out the cadence, but never faster
// than this -- otherwise a snare roll becomes a packet storm.
#define PACKET_MIN_INTERVAL_MS 8

// What the swarm shows when the leader powers on. Looked up BY NAME, not by
// index, so it keeps meaning the same effect even as the registry grows -- an
// index would silently point at something else the next time an effect is
// appended. If the name is not found the leader falls back to index 0 and says
// so, rather than booting into whatever happened to land at that slot.
#ifndef CONDUCTOR_BOOT_SHADER_NAME
#define CONDUCTOR_BOOT_SHADER_NAME "chroma"
#endif

// Shader broadcast. 0 disables auto-cycling; the conductor simply asserts
// shader 0 and the badges stay in lockstep on it.
#ifndef CONDUCTOR_SHADER_CYCLE_MS
#define CONDUCTOR_SHADER_CYCLE_MS 0
#endif
#ifndef CONDUCTOR_SHADER_COUNT
#define CONDUCTOR_SHADER_COUNT 4
#endif

// --- Diagnostics -----------------------------------------------------------
#define SERIAL_REPORT_HZ 4
