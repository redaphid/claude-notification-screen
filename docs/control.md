# Controlling the swarm from a phone

`https://redaphid.github.io/claude-notification-screen/control.html`

Add it to the home screen; it caches itself and works with no network, which is
the only mode that matters at camp.

## What it needs

The leader must be flashed with the BLE console:

```
pio run -e conductor_ble -t upload
```

`-e conductor` (the default leader build) has **no** BLE and will not appear in
the chooser. That is deliberate — see the note in `platformio.ini` for what the
radio costs.

Badges need the firmware from this branch or later to be addressable. Older
badges still follow the leader normally; they simply never appear in the roster
and ignore commands, because both ride frame types their firmware does not know.

## What it does

- **Everyone** — pick the effect the whole swarm shows, step through the list,
  or auto-cycle. Exactly what typing an effect name on the leader's serial
  console does.
- **Leader** — packets per second on the air, whether the mic gate is open, and
  a live energy bar. If the bar moves, the whole chain is alive.
- **Badges** — every badge that has beaconed in the last 15 seconds, what it is
  showing, and its frame rate. Tap one to pin it to a visual, pulse it so you
  can find it in the dark, or let it follow the leader again.

## Knobs

Eight live parameters, the way paper-cranes does it: turn them while the visual
is running instead of editing, rebuilding and reloading. Every turn is
broadcast, so the whole swarm changes together.

Slot meanings are kept the same across effects on purpose, because somebody
poking bytes in a BLE scanner has no labels in front of them:

| knob | means |
|------|-------|
| 1 | reactivity — how far the music is allowed to move this visual |
| 2 | scale / size |
| 3 | speed / spin |
| 4 | hue |
| 5 | glow / depth |
| 6-8 | whatever the effect wants (`kick` on mon and chroma) |

An effect that ignores a slot reports no label for it, and the page hides that
slider rather than showing a control that does nothing.

**Knobs reload their defaults when the effect changes.** Knob 6 is `kick` in mon
and unused in plasma, so carrying values across would leave a visual
mysteriously wrong. Anything you want kept, re-send.

`chroma`'s reactivity defaults to 70 rather than 255. That is the argument in
issue #2 — the paper-cranes work on this artwork concluded loudness must not
move the crest's geometry, or it stops being nameable — turned into something
you can settle by hand. 0 holds the crest perfectly still and lets only colour
answer the room; 255 is the old hard zoom.

## Crests

Eleven family crests. `crest all kikyo` on the console, or the buttons on the
page. With a badge sheet open the buttons address that badge alone.

A crest is remembered in flash, because it is an identity rather than a
setting: a badge that forgets whose it is when the power bank is swapped at 3am
is no use. A crest chosen over the air outranks the one derived from the MAC,
which was only ever a default.

## From a generic BLE scanner

This is the reason the knobs are separate one-byte characteristics rather than
fields in a command frame. Connect with nRF Connect or LightBlue and the
service reads:

```
knob 1 (reactivity)    70    [read,write,notify]
knob 2 (size)         128    [read,write,notify]
knob 3 (spin)         128    [read,write,notify]
knob 4 (hue)            0    [read,write,notify]
knob 5 (depth)        160    [read,write,notify]
knob 6 (kick)         128    [read,write,notify]
knob 7 (unused here)    0    [read,write,notify]
effect (index)          4    [read,write,notify]
crest (index)                [write]
```

Type a byte into any of them and the swarm follows. The labels come from a
0x2901 Characteristic User Description, refreshed whenever the effect changes —
though an app that is already connected will not re-read them, which is the
other reason the slot meanings above are kept consistent.

`scripts/test/leader-gatt-dump.py` prints exactly what such an app sees.

## Browser support, honestly

Web Bluetooth exists in **Chrome on Android** and Chrome/Edge on desktop. It does
**not** exist in Safari or in any browser on iOS. An iPhone cannot run this page;
that is Apple's decision and there is no workaround worth shipping.

## Doing the same thing from a laptop

`scripts/test/leader-ble.py` speaks the identical contract, which is how the byte
layout gets checked without a phone:

```
python3 scripts/test/leader-ble.py                # state + roster
python3 scripts/test/leader-ble.py iris           # the whole swarm
python3 scripts/test/leader-ble.py 85dcdc tunnel  # one badge
python3 scripts/test/leader-ble.py 85dcdc free
python3 scripts/test/leader-ble.py 85dcdc find
python3 scripts/test/leader-gatt-dump.py         # the GATT table, as a scanner sees it
```

And without any hardware at all, the desktop harness takes the same knobs:

```
./harness/build/preview --effect chroma --knobs
./harness/build/preview --effect chroma --knob 1=0 --frames 40 --out /tmp/still
```

## And from the leader's serial console

The same verbs, because the BLE handler calls the same two functions:

```
who                     the badges heard from, and what each is showing
rollcall                ask everyone to announce themselves now
pin <id|all> <effect>   hold a badge on one visual
free <id|all>           let it follow the leader again
find <id|all> [secs]    pulse a white ring
dim <id|all> <0-255>    backlight
knobs                   this effect's knobs, their values and defaults
knob <1-8> <0-255>      turn one, everywhere
reset                   knobs back to this effect's defaults
crests                  list the family crests
crest <id|all> <name>   which crest a badge wears (remembered in flash)
```

A badge's id is the last three bytes of its MAC, which it prints at boot
(`[badge] id 85dcdc`) and wears in the roster.

## The button on the back

- **tap** — next effect, and stay on it.
- **hold 1.2s** — let go, follow the leader again.

A tap pins deliberately. A badge whose wearer picked a visual keeps it even
though the leader is still shouting a different one thirty times a second.

A pin from the button or the phone outranks `BADGE_LOCK_EFFECT`, so the bag
builds (`badge_mon`, `badge_chroma`) choose a badge's *default* rather than
forbidding a wearer from changing their mind.

## Nothing is written on the visuals

Badges and the leader draw the effect and nothing else. Every number the old
HUD carried lives somewhere better now: the roster on the phone knows what each
badge is showing, whether it is pinned and whether it is hearing the leader; the
serial line carries fps, packet counts and the neighbour table; and `find`
pulses a ring when you need to pick one badge out of thirty.

The leader's white beat ring went with it, for a second reason -- `mon.c` is
explicit that ChromaDepth wants no white anywhere, because white is every
wavelength at once and the glasses smear it.

For bench work, where a photograph of the screen being a readable status report
matters more than the visual does:

```
PLATFORMIO_BUILD_FLAGS=-DBADGE_HUD=1  pio run -t upload
PLATFORMIO_BUILD_FLAGS=-DLEADER_HUD=1 pio run -e conductor -t upload
```

The one thing still written on the panel is the boot self-test -- the RED /
GREEN / BLUE / SPRITE RED cards, about four seconds at power-on. That is not a
visual, it is the only way to tell a dead backlight from a dead panel from a
wrong colour order without instruments, and it is gone before the first effect
draws. Say the word and it goes too.

## What is not verified

Everything above was measured at desk range, one badge, one leader. BLE range
and what modem sleep costs the roster across a field are unmeasured — issue #7.
