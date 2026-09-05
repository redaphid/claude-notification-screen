// Stage 2: the conductor.
//
// Exactly one badge in the swarm has a microphone. It listens to the room, runs
// the analysis, and broadcasts a 24-byte ChorusPacket at 30 Hz. Every other badge
// is deaf: it receives, relays, and renders. Twenty independent listeners would
// produce twenty slightly different analyses, which an audience reads as broken.
// Redundancy comes from role, not from hardware.
//
// Build:
//   pio run -e conductor        real I2S mic (ESP32-S3-Touch-LCD-1.46)
//   pio run -e conductor_fake   synthesised input, runs on any ESP32-S3
//
// ---------------------------------------------------------------------------
// WHY THE PACKET HAS NO BEAT FLAG, AND WHAT WE DO INSTEAD
// ---------------------------------------------------------------------------
// ChorusPacket is frozen at 24 bytes and has no triggers bitfield, so an onset
// cannot be announced directly. It does not have to be. The old browser version
// had roughly a second of lag, and the reflex was to blame transport -- but the
// lag was self-inflicted: one signal was being asked to do two incompatible
// jobs. As a MEASUREMENT it wants fidelity, which means responding instantly and
// shuddering. As an ANIMATION DRIVER it wants continuity, which means not
// shuddering. Smoothing was applied to fix the shudder, and the smoothing was
// the lag.
//
// The fix is to stop conflating them. Measure sharply (short hop, spectral flux,
// adaptive threshold), detect the EVENT, and then SYNTHESISE the response: an
// onset slams the affected feature envelopes to full in a single frame and
// releases them on a designed curve. What crosses the radio is already the
// animation, so badges never smooth anything and never add lag of their own.
//
// The side effect is that badges can recover the beat approximately for free: a
// conductor-originated onset always produces a one-packet jump in `energy` to
// 1.0. A receiver-side rising-edge test --
//     beat = (energy - prev_energy) > 0.25
// -- reconstructs `EffectInput.beat`, and `energy` itself is already the
// attack-decay shape `EffectInput.beat_env` wants. It is approximate, and a real
// trigger byte would be better. See docs/chorus-packet-v2-proposal.md for the
// layout to adopt when the contract is next opened; it is NOT implemented here.
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "conductor_config.h"
#include "conductor_display.h"
#include "dsp.h"
#include "mic_source.h"
#include "net_espnow.h"

static MicSource mic;
static AudioAnalyzer analyzer;
static ChorusRadio radio;

static float hopBuf[HOP_SIZE];
static bool micOk = false;
static bool radioOk = false;

static uint32_t lastPacketMs = 0;
static uint32_t lastReportMs = 0;
static uint32_t framesSinceReport = 0;
static uint8_t currentShader = 0;
static uint32_t lastShaderMs = 0;
static uint32_t lastRadioTryMs = 0;

// Analysis cost, measured on the device rather than guessed. The latency budget
// in the report is arithmetic everywhere except here, so print the real number
// and let bring-up confirm or refute it.
static uint32_t analysisUsSum = 0;
static uint32_t analysisUsMax = 0;
static bool displayOk = false;
static uint32_t displayFps = 0;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  Serial.println();
  Serial.println("[conductor] boot");
  Serial.printf("[conductor] %d Hz, FFT %d (%0.1f ms window), hop %d (%0.1f ms, %.1f fps)\n",
                AUDIO_SAMPLE_RATE, FFT_SIZE, 1000.0f * FFT_SIZE / AUDIO_SAMPLE_RATE, HOP_SIZE,
                1000.0f * FRAME_DT_S, FRAME_RATE_HZ);
  Serial.printf("[conductor] high-pass %0.0f Hz, bands %0.0f-%0.0f / %0.0f-%0.0f / %0.0f-%0.0f Hz\n",
                HPF_CUTOFF_HZ, BAND_BASS_LO, BAND_BASS_HI, BAND_MID_LO, BAND_MID_HI,
                BAND_TREBLE_LO, BAND_TREBLE_HI);

  analyzer.init();

  micOk = mic.begin();
  Serial.printf("[conductor] source: %s%s\n", mic.sourceName(), micOk ? "" : "  <-- FAILED");

  radioOk = radio.begin();
  if (!radioOk) {
    Serial.printf("[conductor] radio did not come up; analysing anyway, retrying every %d ms\n",
                  RADIO_RETRY_MS);
  }

  lastPacketMs = millis();
  lastReportMs = millis();
  lastShaderMs = millis();
  lastRadioTryMs = millis();

  // The leader has a screen too. It shows what this board is hearing, so a
  // silent mic or a mute radio is visible from across a room without a laptop.
  displayOk = conductorDisplayInit();
  if (!displayOk) {
    Serial.println("[conductor] no display -- continuing headless, audio still runs");
  }
}

void loop() {
  if (!micOk) {
    // Fail loud and slow rather than spinning silently at full tilt.
    static uint32_t lastWhine = 0;
    if (millis() - lastWhine > 2000) {
      lastWhine = millis();
      Serial.println("[conductor] no audio source -- check the PIN BLOCK in conductor_config.h");
    }
    delay(50);
    return;
  }

  if (!mic.readHop(hopBuf)) return;

  uint32_t t0 = micros();
  const AudioFeatures &f = analyzer.pushHop(hopBuf);
  uint32_t dtUs = micros() - t0;
  analysisUsSum += dtUs;
  if (dtUs > analysisUsMax) analysisUsMax = dtUs;
  framesSinceReport++;

#if CONDUCTOR_SHADER_CYCLE_MS > 0
  if (millis() - lastShaderMs >= (uint32_t)CONDUCTOR_SHADER_CYCLE_MS) {
    lastShaderMs = millis();
    currentShader = (uint8_t)((currentShader + 1) % CONDUCTOR_SHADER_COUNT);
    Serial.printf("[conductor] shader -> %u\n", (unsigned)currentShader);
  }
#endif

  // Cadence is 30 Hz, but an onset does not wait its turn: the whole point of
  // detecting the event is that the response goes out on the beat, not up to
  // 33 ms after it. The floor keeps a snare roll from becoming a packet storm.
  uint32_t now = millis();
  uint32_t sinceTx = now - lastPacketMs;
  bool due = sinceTx >= PACKET_INTERVAL_MS;
  bool urgent = f.onset && sinceTx >= PACKET_MIN_INTERVAL_MS;

  if (radioOk && (due || urgent)) {
    radio.broadcast(currentShader, f.bass, f.mid, f.treble, f.energy);
    lastPacketMs = now;
  } else if (!radioOk && (now - lastRadioTryMs) >= (uint32_t)RADIO_RETRY_MS) {
    // A rail that sagged during the first bring-up may well be fine now. Keep
    // listening and analysing throughout; a mute conductor is recoverable, a
    // conductor that halted in setup() is a walk back to the tent.
    lastRadioTryMs = now;
    Serial.println("[conductor] retrying radio bring-up");
    radioOk = radio.begin();
    lastPacketMs = now;
  }

  // The panel is 412x412 over 40MHz QSPI, so a frame costs real time. Draw at
  // a fixed cadence rather than once per analysis hop (62/s), which would eat
  // the audio budget for frames nobody can perceive.
  if (displayOk) {
    // Publishing is a few stores under a spinlock; the frame itself is drawn on
    // the other core, so this costs the audio path essentially nothing.
    const float feats[4] = {f.bass, f.mid, f.treble, f.energy};
    conductorDisplayDraw(feats, f.onset ? 1 : 0, f.beatEnv, radio.sent(), displayFps);
    displayFps = conductorDisplayFps();

    static uint32_t lastDrawLogMs = 0;
    if (now - lastDrawLogMs >= 3000) {
      lastDrawLogMs = now;
      Serial.printf("[leader] draw %luus, %lu draw-fps\n",
                    (unsigned long)conductorDisplayLastDrawUs(), (unsigned long)displayFps);
    }
  }

  if (now - lastReportMs >= (uint32_t)(1000 / SERIAL_REPORT_HZ)) {
    float dt = (now - lastReportMs) / 1000.0f;
    float avgUs = framesSinceReport ? (float)analysisUsSum / framesSinceReport : 0.0f;
    Serial.printf(
        "[c] b%.2f m%.2f t%.2f e%.2f | raw %5.2f %5.2f %5.2f %5.2f | flux %.3f/%.3f%s "
        "gate%.2f beat%.2f | %.0f fps  dsp %.0f/%luus  tx%lu ech%lu(h%u)\n",
        f.bass, f.mid, f.treble, f.energy, f.rawDb[0], f.rawDb[1], f.rawDb[2], f.rawDb[3], f.flux,
        f.fluxThreshold, f.onset ? " *" : "  ", f.gate, f.beatEnv, framesSinceReport / dt, avgUs,
        (unsigned long)analysisUsMax, (unsigned long)radio.sent(),
        (unsigned long)ChorusRadio::echoesHeard(), (unsigned)ChorusRadio::lastEchoHop());
    lastReportMs = now;
    framesSinceReport = 0;
    analysisUsSum = 0;
    analysisUsMax = 0;
  }
}
