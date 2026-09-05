#include "effects.h"

const Effect *const effects_all[] = {
    &effect_plasma,
    &effect_tunnel,
    &effect_iris,
    &effect_mon,
    &effect_chroma,
};

const int effects_count = (int)(sizeof(effects_all) / sizeof(effects_all[0]));

const Effect *effects_by_index(int index) {
  if (index < 0) index = 0;
  return effects_all[index % (int)(sizeof(effects_all) / sizeof(effects_all[0]))];
}
