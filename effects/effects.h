// The registry of shipped effects. Firmware and the desktop harness both
// include this and iterate effects_all[] -- the ChorusPacket `shader` byte
// indexes straight into it, so "everyone switch to 3" needs no table on the
// firmware side.
#pragma once

#include "effect.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const Effect effect_plasma;
extern const Effect effect_tunnel;
extern const Effect effect_iris;
extern const Effect effect_mon;
extern const Effect effect_lattice;
extern const Effect effect_nfold;
extern const Effect effect_satellites;
extern const Effect effect_chroma;  // mon with a ChromaDepth palette

// mon extras (not part of the frozen effect.h contract). A badge picks which
// crest it wears; unselected, mon cycles through all of them.
void mon_select(int variant);
int mon_variant_count(void);
const char *mon_variant_name(int variant);
// For the other crest effects (lattice, nfold, satellites): which crest this
// badge wears (-1 = none selected, cycle) and the hue that crest always gets,
// so a bead is the same colour whichever effect the leader picks.
int mon_selected(void);
uint8_t mon_hue(int variant);

// Ordered; index == ChorusPacket.shader. Append only -- reordering changes
// what every badge in the swarm shows.
extern const Effect *const effects_all[];
extern const int effects_count;

// Wraps out-of-range shader bytes so a corrupt packet cannot pick garbage.
const Effect *effects_by_index(int index);

#ifdef __cplusplus
}
#endif
