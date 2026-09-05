# Desktop preview harness for effects/.
#
#   make                      build harness/build/preview
#   make gif EFFECT=plasma    render + assemble outputs/plasma.gif
#   make all-gifs             every effect in the registry
#   make bench                per-frame render timings for every effect
#   make clean
#
# SDL is not used and must not be added: this has to run headless. ffmpeg does
# the GIF assembly from PPM frames.

ROOT     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
EFFECTS  := $(ROOT)/effects
HARNESS  := $(ROOT)/harness
BUILD    := $(HARNESS)/build
OUT      := $(ROOT)/outputs

CC      ?= cc
# _POSIX_C_SOURCE is for clock_gettime in the harness only; the effect sources
# themselves stay strict C99 (see the `strict` target).
CFLAGS  ?= -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra \
           -Wno-unused-parameter -I$(EFFECTS) -I$(HARNESS)
LDLIBS  ?= -lm

SRC := $(EFFECTS)/effect_common.c $(EFFECTS)/effects.c $(EFFECTS)/plasma.c \
       $(EFFECTS)/tunnel.c $(EFFECTS)/iris.c \
       $(EFFECTS)/mon.c $(EFFECTS)/mon_data.c \
       $(HARNESS)/mock_dj.c $(HARNESS)/preview.c
OBJ := $(patsubst $(ROOT)/%.c,$(BUILD)/%.o,$(SRC))
BIN := $(BUILD)/preview

# Render knobs. 150 frames at 30fps is 5s of timeline == ~10 beats at 118 BPM,
# enough to see the beat envelope fire and settle several times.
EFFECT ?= plasma
FRAMES ?= 150
FPS    ?= 30
BPM    ?= 118
# GIF plays back at 25fps to keep the file small; the render is still 30fps.
GIF_FPS ?= 25

.PHONY: all gif all-gifs bench clean list strict

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

list: $(BIN)
	@$(BIN) --list

# ---------------------------------------------------------------------------
# GIF assembly. Two ffmpeg passes: palettegen over the whole clip so colours
# do not swim between frames, then paletteuse with a Bayer dither, which
# compresses far better than error diffusion on this kind of smooth field.
# ---------------------------------------------------------------------------
gif: $(BIN)
	@mkdir -p $(OUT) $(BUILD)/frames/$(EFFECT)
	@rm -f $(BUILD)/frames/$(EFFECT)/*.ppm
	$(BIN) --effect $(EFFECT) --frames $(FRAMES) --fps $(FPS) --bpm $(BPM) \
	       --out $(BUILD)/frames/$(EFFECT)
	@ffmpeg -y -loglevel error \
	  -framerate $(FPS) -i $(BUILD)/frames/$(EFFECT)/frame_%05d.ppm \
	  -filter_complex "fps=$(GIF_FPS),split[a][b];[a]palettegen=max_colors=192:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3" \
	  -loop 0 $(OUT)/$(EFFECT).gif
	@rm -f $(BUILD)/frames/$(EFFECT)/*.ppm
	@echo "wrote $(OUT)/$(EFFECT).gif ($$(du -h $(OUT)/$(EFFECT).gif | cut -f1))"

all-gifs: $(BIN)
	@for e in $$($(BIN) --list); do $(MAKE) --no-print-directory -f $(HARNESS)/Makefile gif EFFECT=$$e; done

bench: $(BIN)
	@for e in $$($(BIN) --list); do $(BIN) --effect $$e --bench 400; echo; done

# Proves the effect sources really are portable C99 with no platform headers,
# which is the precondition for the same files compiling for the badge.
strict:
	@mkdir -p $(BUILD)
	$(CC) -std=c99 -pedantic-errors -Wall -Wextra -Werror -O2 -I$(EFFECTS) -fsyntax-only \
	  $(EFFECTS)/effect_common.c $(EFFECTS)/effects.c \
	  $(EFFECTS)/plasma.c $(EFFECTS)/tunnel.c $(EFFECTS)/iris.c
	@echo "effects/ are strict C99"

clean:
	rm -rf $(BUILD)
