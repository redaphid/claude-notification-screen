// BLE side of the phone-to-badge link: advertises the Chorus service, serves
// the catalog, takes control writes from a phone, and pushes status back.
//
// Threading: NimBLE callbacks run on the BLE host task. Nothing here touches
// the display or the effects; a write is parked and main.cpp collects it from
// loop() through bleControlPoll(), applies it, then publishes the result.
#pragma once

#include <stdint.h>

#include "phone_control.h"

// Start the GATT server and advertise. `name` is what a phone sees in its
// picker ("Chorus-29D0"); `catalog` is served read-only from PHONE_CATALOG_UUID;
// `initial` is what the control characteristic reads until the first change.
void bleControlInit(const char *name, const char *catalog, const PhoneControlFrame &initial);

// From loop(): true, and *out filled, once per write a phone made.
bool bleControlPoll(PhoneControlFrame *out);

// After main.cpp applied a change: update the readable value and notify.
void bleControlPublish(const PhoneControlFrame &now);

// About once a second from loop(): what the phone page shows about this badge.
void bleControlStatus(const PhoneStatusFrame &st);

bool bleControlConnected();
