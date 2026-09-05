// Phone-to-badge BLE contract, part two: choosing what a badge shows.
//
// Same GATT service as phone_link.h (which carries features from a phone that
// is conducting). These two characteristics let a phone pick the visual on the
// badge it is connected to -- the effect, the crest for the "mon" effect, the
// backlight -- and optionally remember that choice across power cycles. The
// features still come from whoever is conducting; this only chooses the look.
//
// Keep web/control.html in step with this file: it reads the same UUIDs and
// the same byte layout.
#pragma once

#include <stdint.h>

#include "phone_link.h"

// Phone <-> badge. Read returns the current selection, write changes it, notify
// fires after the badge applied a change (so the page reflects what actually
// happened, not what it asked for). Short writes are accepted: 1 byte sets
// the effect only, 2 bytes effect + crest, and so on; omitted fields keep
// their current value.
#define PHONE_CONTROL_UUID "c8a0f103-0451-4000-b000-63726e730001"

// Badge -> phone, read only. ASCII, e.g.
//   "name=Chorus-29D0;effects=plasma,tunnel,iris,mon;crests=kiku,tomoe,...;mon=3"
// so a page can build its menus from the firmware instead of a copy of it.
#define PHONE_CATALOG_UUID "c8a0f104-0451-4000-b000-63726e730001"

#define PHONE_CONTROL_FOLLOW 255    // effect: follow the conductor's shader byte
#define PHONE_CONTROL_DEFAULT 255   // crest: this badge's own MAC-keyed crest
#define PHONE_CONTROL_FLAG_PERSIST 0x01

struct __attribute__((packed)) PhoneControlFrame {
  uint8_t effect;      // index into effects_all[], or PHONE_CONTROL_FOLLOW
  uint8_t crest;       // mon variant index, or PHONE_CONTROL_DEFAULT
  uint8_t brightness;  // backlight 0..255
  uint8_t flags;       // PHONE_CONTROL_FLAG_PERSIST: remember in flash
};

static_assert(sizeof(PhoneControlFrame) == 4, "PhoneControlFrame is a wire contract");
