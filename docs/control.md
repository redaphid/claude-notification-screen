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

## What is not verified

Everything above was measured at desk range, one badge, one leader. BLE range
and what modem sleep costs the roster across a field are unmeasured — issue #7.
