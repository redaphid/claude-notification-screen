// Desktop preview harness.
//
// Renders N frames of one effect, driven by the mock DJ, into PPM files that
// ffmpeg assembles into a GIF. SDL is deliberately not used: this has to run
// headless over ssh, and "watch the GIF" is a better review loop than "run the
// window" when the person reviewing is remote.
//
// It also times render() alone (not the PPM write, not the DJ) and prints a
// distribution. Those numbers are x86 numbers and DO NOT transfer to the
// badge: a desktop core is ~10x the clock, several-issue, and has a cache
// bigger than the whole framebuffer. Treat the desktop timing as a regression
// signal ("did this change make it 3x worse") and read the per-pixel
// instruction count out of the xtensa object files for the real budget:
//   xtensa-esp32s3-elf-objdump -d .pio/build/*/effects/plasma.c.o
//
//   preview --effect plasma --frames 150 --fps 30 --out outputs/frames/plasma
//   preview --effect iris --bench 400
//   preview --list
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../effects/effects.h"
#include "../effects/knobs.h"
#include "mock_dj.h"

// Start close to the uint32 wrap so a normal 150-frame run rolls time_ms over
// mid-render. Effects that mishandle the wrap fail here instead of at 3am on
// day 49 of the season.
#define DEFAULT_T0 0xFFFFF000u

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static const Effect *find_effect(const char *name) {
  for (int i = 0; i < effects_count; ++i)
    if (strcmp(effects_all[i]->name, name) == 0) return effects_all[i];
  return NULL;
}

static int write_ppm(const char *dir, int index, const uint16_t *fb) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dir, index);
  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "preview: cannot write %s\n", path);
    return -1;
  }
  fprintf(f, "P6\n%d %d\n255\n", EFFECT_W, EFFECT_H);
  static uint8_t row[EFFECT_W * 3];
  for (int y = 0; y < EFFECT_H; ++y) {
    for (int x = 0; x < EFFECT_W; ++x) {
      uint8_t r, g, b;
      effect_unpack565(fb[y * EFFECT_W + x], &r, &g, &b);
      // effect_rgb565 truncates; put the low bits back so flat gradients on
      // the desktop preview look like the panel rather than like banding the
      // effect did not actually produce.
      row[x * 3 + 0] = (uint8_t)(r | (r >> 5));
      row[x * 3 + 1] = (uint8_t)(g | (g >> 6));
      row[x * 3 + 2] = (uint8_t)(b | (b >> 5));
    }
    fwrite(row, 1, sizeof(row), f);
  }
  fclose(f);
  return 0;
}

// effect.h: "pixels outside this radius are never seen. Effects may skip them,
// but must leave them black rather than undefined." The framebuffer is poisoned
// before the first render, so this catches both an unwritten pixel and a
// scanline span computed one column too narrow.
static int check_round_mask(const uint16_t *fb) {
  const float cx = (float)EFFECT_W * 0.5f - 0.5f;
  const float cy = (float)EFFECT_H * 0.5f - 0.5f;
  const float r2 = (float)EFFECT_RADIUS * (float)EFFECT_RADIUS;
  int bad = 0;
  for (int y = 0; y < EFFECT_H; ++y) {
    for (int x = 0; x < EFFECT_W; ++x) {
      const float dx = (float)x - cx, dy = (float)y - cy;
      // One pixel of slack at the boundary: whether the rim pixel itself is
      // inside is a rounding question, not a contract violation.
      if (dx * dx + dy * dy <= r2 + 2.0f * (float)EFFECT_RADIUS) continue;
      if (fb[y * EFFECT_W + x] != 0) {
        if (++bad <= 3)
          fprintf(stderr, "preview: pixel (%d,%d) outside the disc is 0x%04x, not black\n", x, y,
                  fb[y * EFFECT_W + x]);
      }
    }
  }
  if (bad) fprintf(stderr, "preview: %d off-disc pixels not black\n", bad);
  return bad;
}

static void usage(void) {
  fprintf(stderr,
          "usage: preview [--effect NAME] [--frames N] [--fps F] [--bpm B]\n"
          "               [--out DIR] [--bench N] [--list] [--knobs]\n"
          "               [--knob N=V ...]   N is 1..%d, V is 0..255\n",
          KNOB_COUNT);
}

int main(int argc, char **argv) {
  const char *name = "plasma";
  const char *outdir = NULL;
  int frames = 150;
  double fps = 30.0;
  float bpm = 118.0f;
  int bench = 0;
  int show_knobs = 0;
  const char *setk[KNOB_COUNT];
  int nset = 0;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--list")) {
      for (int e = 0; e < effects_count; ++e) printf("%s\n", effects_all[e]->name);
      return 0;
    } else if (!strcmp(argv[i], "--effect") && i + 1 < argc) {
      name = argv[++i];
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      outdir = argv[++i];
    } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      frames = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
      fps = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--bpm") && i + 1 < argc) {
      bpm = (float)atof(argv[++i]);
    } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
      bench = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--knobs")) {
      show_knobs = 1;
    } else if (!strcmp(argv[i], "--knob") && i + 1 < argc) {
      if (nset < (int)(sizeof(setk) / sizeof(setk[0]))) setk[nset++] = argv[++i];
      else ++i;
    } else {
      usage();
      return 2;
    }
  }

  if (bench > 0) frames = bench;
  if (frames <= 0 || fps <= 0.0) { usage(); return 2; }

  const Effect *fx = find_effect(name);
  if (!fx) {
    fprintf(stderr, "preview: no effect named '%s'. known:", name);
    for (int e = 0; e < effects_count; ++e) fprintf(stderr, " %s", effects_all[e]->name);
    fprintf(stderr, "\n");
    return 2;
  }

  // Knobs default to what this effect declares, then the command line
  // overrides. Same order the firmware applies them in, so a value that looks
  // right here looks right on a badge.
  {
    int idx = 0;
    for (int e = 0; e < effects_count; ++e)
      if (effects_all[e] == fx) idx = e;
    knobs_reset_for(idx);
    for (int i = 0; i < nset; ++i) {
      int n = 0, v = 0;
      if (sscanf(setk[i], "%d=%d", &n, &v) == 2 && n >= 1 && n <= KNOB_COUNT) {
        knob_set(n - 1, (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)));
      } else {
        fprintf(stderr, "preview: bad --knob '%s' (want N=V)\n", setk[i]);
        return 2;
      }
    }
    if (show_knobs) {
      const KnobSpec *spec = effect_knob_specs(idx);
      printf("%s knobs:\n", fx->name);
      for (int i = 0; i < KNOB_COUNT; ++i) {
        if (!spec[i].name) continue;
        printf("  %d %-12s %3u\n", i + 1, spec[i].name, knob_raw(i));
      }
      return 0;
    }
  }

  uint16_t *fb = (uint16_t *)malloc((size_t)EFFECT_PIXELS * sizeof(uint16_t));
  double *times = (double *)malloc((size_t)frames * sizeof(double));
  if (!fb || !times) { fprintf(stderr, "preview: out of memory\n"); return 1; }
  memset(fb, 0xAA, (size_t)EFFECT_PIXELS * sizeof(uint16_t));  // poison: render must
                                                               // write every pixel

  fx->init();

  MockDj dj;
  mock_dj_init(&dj, bpm, DEFAULT_T0);

  const double frame_ms = 1000.0 / fps;
  int beats = 0;
  int mask_fail = 0;

  for (int i = 0; i < frames; ++i) {
    EffectInput in;
    mock_dj_frame(&dj, (double)i * frame_ms, &in);
    if (in.beat) beats++;

    const double t0 = now_ms();
    fx->render(fb, &in);
    times[i] = now_ms() - t0;

    if (i == 0) mask_fail = check_round_mask(fb);
    if (outdir && write_ppm(outdir, i, fb) != 0) return 1;
  }

  qsort(times, (size_t)frames, sizeof(double), cmp_double);
  double sum = 0.0;
  for (int i = 0; i < frames; ++i) sum += times[i];

  printf("effect      %s\n", fx->name);
  printf("frames      %d @ %.1f fps (%.2f s of timeline, %d beats at %.0f BPM)\n", frames, fps,
         (double)frames * frame_ms / 1000.0, beats, (double)bpm);
  printf("render ms   min %.3f  p50 %.3f  mean %.3f  p95 %.3f  max %.3f\n", times[0],
         times[frames / 2], sum / (double)frames, times[(frames * 95) / 100], times[frames - 1]);
  printf("throughput  %.1f Mpx/s at p50\n",
         (double)EFFECT_PIXELS / (times[frames / 2] * 1000.0));
  if (outdir) printf("frames      %s/frame_%%05d.ppm\n", outdir);

  printf("round mask  %s\n", mask_fail ? "FAILED" : "ok (off-disc pixels black)");

  free(times);
  free(fb);
  return mask_fail ? 1 : 0;
}
