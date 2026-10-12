# Counterpuck v0.1.1

An open firmware that makes an **nRF52840 dev board** (SuperMini / Pro Micro with the Adafruit UF2
bootloader) behave like a Steam Controller 2 puck.

**Unofficial, not made or endorsed by Valve.** It presents Valve's USB IDs so Steam recognises it:
for personal use and research on your own hardware, not for sale. Use at your own risk. It has not
been tried on a real puck and is not meant for one.

## What's new since v0.1.0

- **No stuck buttons.** When a controller's link ends (switched off, out of range), the PC now gets a
  final neutral input report. Before, the last buttons and sticks stayed "held" until the controller
  came back. Checked with buttons and a trigger held while the controller was switched off.
- **Input reports never make the radio wait.** Each interface queues up to eight reports and drops
  one rather than stalling the radio loop if the PC is slow.
- **Channel choice follows the documented behaviour:** old error rates fade by 10% a minute (they could
  make an idle channel look permanently bad), the noise penalty curve is closer to the real puck's, and
  connection attempts run on a steady 4 ms clock starting at once.
- **One combined state** for controllers that send two half-reports, served to the lowest connected
  slot, as the specification says.
- Fixes from two independent code reviews: a recoverable radio timeout no longer risks a reset, the
  noise scan has bounded waits and cannot match packets, flash writes keep the radio quiet, a failed
  pairing save no longer leaves an empty pairing, malformed commands are rejected, and a race that could
  tell a controller to switch off just as the PC woke is closed.
- New shell commands: `trace_output` (log the output reports Steam sends, such as rumble) and `records`
  (count the input reports a controller sends).

## Install

1. Double-tap the board's reset button so a drive appears.
2. Copy `counterpuck-v0.1.1-supermini-nrf52840.uf2` onto it. The board restarts as a puck.
3. In Steam, pair a controller as usual (or run `pair 0` in the board's serial shell).

Upgrading from v0.1.0 keeps your pairing. Check the download against `SHA256SUMS`. Settings (serial
numbers, build time reported to Steam, pairing window, radio power) are compile-time options in
`firmware/src/puck_config.h`.

## What works (tested with a real Steam Controller 2)

- the USB device a puck presents, and every vendor command Steam sends it;
- pairing, connecting, protocol version 1, channel switching, link quality reports;
- input at about 270 reports per second; buttons, sticks and trackpads;
- rumble, PC sleep and wake (the controller is switched off when the PC sleeps, and reconnects when it
  wakes), and pairings kept across reflashing.

## Known issues

- After the PC sleeps, the serial shell stays silent until the board is rebooted or replugged. The
  controller link is not affected.
- A rare "no event from the radio" error can appear in the log, at worst about once a minute. It
  recovers by itself and has no visible effect; its cause is not found.
- Not tested: a sleep of more than a few seconds; more than one controller at once.
- Steam's own "update" or "downgrade" for a puck is refused (the board's bootloader does not speak
  Steam's update protocol): see `PUCK_STEAM_MAY_REBOOT_TO_BOOTLOADER`.

## Licences

MIT for this project's own code. The image also contains Zephyr (Apache-2.0), nrfx (BSD-3-Clause) and
Nordic's Enhanced ShockBurst library (Nordic 5-clause licence, text in `LICENSE-Nordic-5-Clause.txt`:
use on Nordic chips only). Details: `firmware/THIRD_PARTY.md`.
