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

#include <string.h>

#include "conductor_config.h"
#include "conductor_display.h"
// Relative on purpose: -I${PROJECT_DIR}/effects in platformio.ini comes out
// mangled on Windows builds, and the display file already includes this way.
#include "../effects/effects.h"
#include "../effects/knobs.h"
#include "dsp.h"
#include "mic_source.h"
#include "leader_link.h"
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
static bool bleUp = false;
static uint32_t displayFps = 0;

// Auto-cycle period, adjustable at runtime; 0 holds the current shader.
static uint32_t shaderCycleMs = CONDUCTOR_SHADER_CYCLE_MS;

// The leader is the one place a human can choose what the whole swarm shows:
// its shader byte goes out in every packet and every badge follows it. So the
// leader takes commands on its serial console, one per line:
//   shader <n> | s <n> | <n>   pick an effect by index
//   plasma | tunnel | iris | mon ...   pick by name (any name in effects_all[])
//   next | n  |  prev | p      step through the list
//   cycle <ms>                 auto-advance every <ms> (0 = hold)
//   ? | help                   list effects and the current one
static void setShader(int index) {
  if (effects_count <= 0) return;
  index = ((index % effects_count) + effects_count) % effects_count;
  currentShader = (uint8_t)index;
  lastShaderMs = millis();
  conductorDisplaySetShader(currentShader);
  // Knob 6 is "kick" in mon and unused in plasma, so values do not carry
  // across an effect change -- they load that effect's declared defaults, and
  // anything listening is told the new labels and values.
  knobs_reset_for(currentShader);
  leaderLinkPublishKnobs(currentShader);
  Serial.printf("[conductor] shader -> %u (%s)\n", (unsigned)currentShader,
                effects_all[currentShader]->name);
}

// --- addressing one badge -----------------------------------------------
// A badge answers to the last three bytes of its MAC, printed as six hex digits
// on its own serial line ("[badge] id 85dcdc"). "all" is the whole swarm.
static bool parseBadgeId(const String &text, uint8_t out[3]) {
  if (text == "all" || text == "*") {
    out[0] = out[1] = out[2] = 0;
    return true;
  }
  if (text.length() != 6) return false;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    const char c = text[i];
    uint8_t nib;
    if (c >= '0' && c <= '9') nib = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') nib = (uint8_t)(c - 'a' + 10);
    else return false;
    v = (v << 4) | nib;
  }
  chorusTailToId(v, out);
  return true;
}

// Effect by name or index, so the console and the phone agree on what "chroma"
// means without either hardcoding a number.
static int parseEffect(const String &text) {
  for (int i = 0; i < effects_count; i++) {
    if (text == effects_all[i]->name) return i;
  }
  if (!text.isEmpty() && isDigit(text[0])) {
    const int n = text.toInt();
    if (n >= 0 && n < effects_count) return n;
  }
  return -1;
}

// A knob turn has to reach two places: the leader's own panel, which renders
// the same effect from the same code, and every badge. Broadcast, because the
// point is that the swarm changes together.
static void setKnob(int index, uint8_t value, const uint8_t target[3]) {
  if (index < 0 || index >= KNOB_COUNT) return;
  if (chorusIdIsBroadcast(target)) knob_set(index, value);
  radio.command(CMD_SET_KNOB, target, (uint8_t)index, value);
}

static void printKnobs() {
  const KnobSpec *spec = effect_knob_specs(currentShader);
  Serial.printf("[knobs] %s:\n", effects_all[currentShader]->name);
  for (int i = 0; i < KNOB_COUNT; i++) {
    if (!spec[i].name) continue;
    Serial.printf("  %d %-12s %3u (default %u)\n", i + 1, spec[i].name, (unsigned)knob_raw(i),
                  (unsigned)spec[i].def);
  }
}

static void printRoster() {
  radio.rosterExpire(ROSTER_STALE_MS);
  const int n = radio.rosterCount();
  Serial.printf("[swarm] %d badge%s\n", n, n == 1 ? "" : "s");
  const uint32_t now = millis();
  for (int i = 0; i < n; i++) {
    const ChorusRadio::RosterEntry *e = radio.rosterAt(i);
    if (!e) continue;
    Serial.printf("  %02x%02x%02x  %-8s %s  %3u fps  rx %3u/s  hop %u  up %us  %lus ago\n", e->id[0],
                  e->id[1], e->id[2],
                  (e->shader < (uint8_t)effects_count) ? effects_all[e->shader]->name : "?",
                  (e->flags & HELLO_PINNED) ? "PIN " : "    ", (unsigned)e->fps,
                  (unsigned)e->rxPerSec, (unsigned)e->hop, (unsigned)e->uptimeS,
                  (unsigned long)((now - e->lastSeenMs) / 1000));
  }
}

// Verbs that address the swarm rather than the leader. Split out from
// handleSerialLine so the BLE console can call exactly the same code and the
// two can never drift into meaning different things.
//   who                     list the badges heard from
//   pin <id|all> <effect>   hold a badge on one visual
//   free <id|all>           let it follow the leader again
//   find <id|all> [secs]    pulse a white ring so it can be spotted
//   dim <id|all> <0-255>    backlight
//   rollcall                ask every badge to announce itself now
static bool handleSwarmCommand(const String &line) {
  if (line == "who" || line == "roster") {
    printRoster();
    return true;
  }
  if (line == "knobs" || line == "k") {
    printKnobs();
    return true;
  }
  if (line == "reset") {
    const uint8_t all[3] = {0, 0, 0};
    knobs_reset_for(currentShader);
    radio.command(CMD_RESET_KNOBS, all);
    Serial.println("[knobs] back to this effect's defaults, everywhere");
    printKnobs();
    return true;
  }
  if (line == "crests") {
    Serial.printf("[swarm] %d crests:", mon_variant_count());
    for (int i = 0; i < mon_variant_count(); i++) Serial.printf(" %d=%s", i, mon_variant_name(i));
    Serial.println();
    return true;
  }
  if (line == "rollcall") {
    const uint8_t all[3] = {0, 0, 0};
    radio.command(CMD_ROLL_CALL, all);
    Serial.println("[swarm] roll call sent");
    return true;
  }
  int sp = line.indexOf(' ');
  if (sp < 0) return false;
  const String verb = line.substring(0, sp);
  String rest = line.substring(sp + 1);
  rest.trim();
  sp = rest.indexOf(' ');
  const String who = (sp < 0) ? rest : rest.substring(0, sp);
  String arg = (sp < 0) ? String("") : rest.substring(sp + 1);
  arg.trim();

  // knob takes a number where the others take a badge id, so it is handled
  // before the id parse rather than pretending "3" is a badge.
  if (verb == "knob") {
    const int n = who.toInt();
    const uint8_t all[3] = {0, 0, 0};
    if (n < 1 || n > KNOB_COUNT) {
      Serial.printf("[knobs] knob number must be 1..%d\n", KNOB_COUNT);
      return true;
    }
    const uint8_t v = (uint8_t)constrain(arg.toInt(), 0, 255);
    setKnob(n - 1, v, all);
    const KnobSpec *spec = effect_knob_specs(currentShader);
    Serial.printf("[knobs] %d (%s) -> %u\n", n, spec[n - 1].name ? spec[n - 1].name : "unused",
                  (unsigned)v);
    return true;
  }

  uint8_t target[3];
  if (verb != "pin" && verb != "free" && verb != "find" && verb != "dim" && verb != "crest")
    return false;
  if (!parseBadgeId(who, target)) {
    Serial.printf("[swarm] '%s' is not a badge id (six hex digits, or 'all')\n", who.c_str());
    return true;
  }

  if (verb == "pin") {
    const int fx = parseEffect(arg);
    if (fx < 0) {
      Serial.printf("[swarm] no effect '%s' (try ? for the list)\n", arg.c_str());
      return true;
    }
    radio.command(CMD_SET_EFFECT, target, (uint8_t)fx, 0);
    Serial.printf("[swarm] %s -> %s (held until freed)\n", who.c_str(), effects_all[fx]->name);
    return true;
  }
  if (verb == "free") {
    radio.command(CMD_RELEASE, target);
    Serial.printf("[swarm] %s follows the leader again\n", who.c_str());
    return true;
  }
  if (verb == "find") {
    const uint8_t secs = arg.isEmpty() ? 5 : (uint8_t)constrain(arg.toInt(), 1, 60);
    radio.command(CMD_IDENTIFY, target, secs);
    Serial.printf("[swarm] %s pulsing for %us\n", who.c_str(), (unsigned)secs);
    return true;
  }
  if (verb == "crest") {
    int variant = -1;
    for (int i = 0; i < mon_variant_count(); i++) {
      if (arg == mon_variant_name(i)) variant = i;
    }
    if (variant < 0 && !arg.isEmpty() && isDigit(arg[0])) {
      const int n = arg.toInt();
      if (n >= 0 && n < mon_variant_count()) variant = n;
    }
    if (variant < 0) {
      Serial.printf("[swarm] no crest '%s' (try `crests`)\n", arg.c_str());
      return true;
    }
    radio.command(CMD_SET_CREST, target, (uint8_t)variant);
    Serial.printf("[swarm] %s wears %s\n", who.c_str(), mon_variant_name(variant));
    return true;
  }
  if (verb == "dim") {
    const uint8_t v = (uint8_t)constrain(arg.toInt(), 0, 255);
    radio.command(CMD_BRIGHTNESS, target, v);
    Serial.printf("[swarm] %s brightness %u\n", who.c_str(), (unsigned)v);
    return true;
  }
  return false;
}

// A control frame from the phone. Deliberately routed through setShader() and
// radio.command() -- the same two calls the serial console makes -- so that the
// two consoles can never mean different things by the same word.
static void applyLeaderControl(const LeaderControlFrame &f) {
  switch (f.op) {
    case LEADER_OP_SET_EFFECT:
      if (f.arg0 < (uint8_t)effects_count) setShader(f.arg0);
      break;
    case LEADER_OP_NEXT: setShader(currentShader + 1); break;
    case LEADER_OP_PREV: setShader(currentShader - 1); break;
    case LEADER_OP_CYCLE_SECONDS:
      shaderCycleMs = ((uint32_t)f.arg0 | ((uint32_t)f.arg1 << 8)) * 1000u;
      lastShaderMs = millis();
      Serial.printf("[ble] cycle -> %lu ms\n", (unsigned long)shaderCycleMs);
      break;
    case LEADER_OP_BADGE_EFFECT: radio.command(CMD_SET_EFFECT, f.target, f.arg0, f.arg1); break;
    case LEADER_OP_BADGE_RELEASE: radio.command(CMD_RELEASE, f.target); break;
    case LEADER_OP_BADGE_IDENTIFY: radio.command(CMD_IDENTIFY, f.target, f.arg0 ? f.arg0 : 5); break;
    case LEADER_OP_BADGE_BRIGHTNESS: radio.command(CMD_BRIGHTNESS, f.target, f.arg0); break;
    case LEADER_OP_ROLL_CALL: {
      const uint8_t all[3] = {0, 0, 0};
      radio.command(CMD_ROLL_CALL, all);
      break;
    }
    case LEADER_OP_SET_KNOB:
      setKnob(f.arg0, f.arg1, f.target);
      leaderLinkPublishKnobs(currentShader);
      break;
    case LEADER_OP_RESET_KNOBS: {
      const uint8_t all[3] = {0, 0, 0};
      knobs_reset_for(currentShader);
      radio.command(CMD_RESET_KNOBS, all);
      leaderLinkPublishKnobs(currentShader);
      break;
    }
    case LEADER_OP_SET_CREST:
      radio.command(CMD_SET_CREST, f.target, f.arg0);
      break;
    default: Serial.printf("[ble] unknown op %u\n", (unsigned)f.op); break;
  }
}

static void handleSerialLine(String line) {
  line.trim();
  line.toLowerCase();
  if (line.isEmpty()) return;
  if (line == "?" || line == "help") {
    Serial.printf("[conductor] shader %u of %d:", (unsigned)currentShader, effects_count);
    for (int i = 0; i < effects_count; i++) Serial.printf(" %d=%s", i, effects_all[i]->name);
    Serial.printf("  cycle=%lu ms\n", (unsigned long)shaderCycleMs);
    Serial.println("[conductor] swarm: who | rollcall | pin <id|all> <fx> | free <id|all>");
    Serial.println("[conductor]        find <id|all> [secs] | dim <id|all> <0-255>");
    Serial.println("[conductor] knobs: knobs | knob <1-8> <0-255> | reset");
    Serial.println("[conductor] crest: crests | crest <id|all> <name>");
    return;
  }
  if (handleSwarmCommand(line)) return;
  if (line == "next" || line == "n") { setShader(currentShader + 1); return; }
  if (line == "prev" || line == "p") { setShader(currentShader - 1); return; }
  if (line.startsWith("cycle")) {
    shaderCycleMs = (uint32_t)line.substring(5).toInt();
    lastShaderMs = millis();
    Serial.printf("[conductor] cycle -> %lu ms\n", (unsigned long)shaderCycleMs);
    return;
  }
  String arg = line;
  if (line.startsWith("shader")) arg = line.substring(6);
  else if (line.startsWith("s ")) arg = line.substring(2);
  arg.trim();
  for (int i = 0; i < effects_count; i++) {
    if (arg == effects_all[i]->name) { setShader(i); return; }
  }
  if (!arg.isEmpty() && isDigit(arg[0])) { setShader(arg.toInt()); return; }
  Serial.printf("[conductor] unknown command '%s' (try ?)\n", line.c_str());
}

// What the leader broadcasts until someone types otherwise. Named, so it
// follows the registry; "chroma" so a power cycle at the event brings the
// swarm up on the ChromaDepth crests rather than plasma.
#ifndef CONDUCTOR_DEFAULT_EFFECT_NAME
#define CONDUCTOR_DEFAULT_EFFECT_NAME "chroma"
#endif
static int resolveDefaultShader() {
  for (int i = 0; i < effects_count; i++) {
    if (strcmp(effects_all[i]->name, CONDUCTOR_DEFAULT_EFFECT_NAME) == 0) return i;
  }
  return 0;
}

static void pollSerial() {
  static String pending;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!pending.isEmpty()) handleSerialLine(pending);
      pending = "";
    } else if (pending.length() < 64) {
      pending += c;
    }
  }
}

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

#ifdef CONDUCTOR_SILENT
  // Bench flag: analyse and draw, but put nothing on the air. Used to hand the
  // channel to another bench, or to test the phone-as-conductor path, which can
  // only take over when no real conductor is transmitting.
  radioOk = false;
  Serial.println("[conductor] CONDUCTOR_SILENT -- analysing but not transmitting");
#else
  radioOk = radio.begin();
#endif
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
  // Boot on the effect the event wants, resolved by name.
  {
    int boot = -1;
    for (int i = 0; i < effects_count; i++) {
      if (strcmp(effects_all[i]->name, CONDUCTOR_BOOT_SHADER_NAME) == 0) { boot = i; break; }
    }
    if (boot < 0) {
      Serial.printf("[conductor] boot shader \"%s\" not found -- falling back to %s\n",
                    CONDUCTOR_BOOT_SHADER_NAME, effects_all[0]->name);
      boot = 0;
    }
    setShader(boot);
  }

  displayOk = conductorDisplayInit();
  setShader(resolveDefaultShader());  // after the display exists, so its panel follows too
  if (!displayOk) {
    Serial.println("[conductor] no display -- continuing headless, audio still runs");
  }

  // BLE last: it shares the radio with ESP-NOW, and bringing it up before the
  // swarm is being fed would risk the one job this board has for the sake of a
  // console nobody may connect to.
  bleUp = leaderLinkBegin();
  if (bleUp) leaderLinkPublishKnobs(currentShader);
}

void loop() {
  pollSerial();  // works with or without a microphone

  // The phone's console, drained here rather than in the NimBLE task for the
  // same reason relays are drained in loop() on a badge: esp_now_send() belongs
  // to whoever owns the main loop.
  {
    LeaderControlFrame f;
    while (leaderLinkTakeCommand(&f)) applyLeaderControl(f);
  }

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

  if (shaderCycleMs > 0 && millis() - lastShaderMs >= shaderCycleMs) setShader(currentShader + 1);

  // Cadence is 30 Hz, but an onset does not wait its turn: the whole point of
  // detecting the event is that the response goes out on the beat, not up to
  // 33 ms after it. The floor keeps a snare roll from becoming a packet storm.
  uint32_t now = millis();
  uint32_t sinceTx = now - lastPacketMs;
  static uint32_t hopsSinceTx = 0;
  hopsSinceTx++;
  bool due = hopsSinceTx >= (uint32_t)PACKET_EVERY_N_HOPS;
  bool urgent = f.onset && sinceTx >= PACKET_MIN_INTERVAL_MS;

  if (radioOk && (due || urgent)) {
    radio.broadcast(currentShader, f.bass, f.mid, f.treble, f.energy);
    lastPacketMs = now;
    hopsSinceTx = 0;
#ifndef CONDUCTOR_SILENT
  } else if (!radioOk && (now - lastRadioTryMs) >= (uint32_t)RADIO_RETRY_MS) {
    // A rail that sagged during the first bring-up may well be fine now. Keep
    // listening and analysing throughout; a mute conductor is recoverable, a
    // conductor that halted in setup() is a walk back to the tent.
    lastRadioTryMs = now;
    Serial.println("[conductor] retrying radio bring-up");
    radioOk = radio.begin();
    lastPacketMs = now;
#endif
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

    // Tell the phone what the swarm is doing, on the same tick as the serial
    // line, so the two never disagree about the moment they describe.
    if (bleUp) {
      ChorusRadio::rosterExpire(ROSTER_STALE_MS);
      // Per second, not per report. This block runs at SERIAL_REPORT_HZ, so the
      // difference between two counter reads is a quarter of a second's worth
      // of packets -- publishing it as "packets/s" showed the leader on air at
      // 8/s while it was actually transmitting 32/s, which reads as a broken
      // radio rather than as a broken denominator. Divide by the real elapsed
      // time rather than by the constant, so a late loop does not lie either.
      static uint32_t prevTx = 0;
      const uint32_t tx = radio.sent();
      const uint32_t txPerSec = dt > 0.0f ? (uint32_t)((tx - prevTx) / dt) : 0;
      prevTx = tx;
      leaderLinkPublish(currentShader, (uint8_t)effects_count,
                        (uint8_t)ChorusRadio::rosterCount(), f.gate > 0.5f,
                        (uint16_t)(shaderCycleMs / 1000u), (uint16_t)txPerSec,
                        (uint16_t)(now / 1000u), (uint8_t)(f.bass * 255.0f),
                        (uint8_t)(f.energy * 255.0f));
    }

    lastReportMs = now;
    framesSinceReport = 0;
    analysisUsSum = 0;
    analysisUsMax = 0;
  }
}
