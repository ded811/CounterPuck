# Counterpuck v0.1.2

An open firmware that makes an **nRF52840 dev board** (SuperMini / Pro Micro with the Adafruit UF2
bootloader) behave like a Steam Controller 2 puck.

**Unofficial, not made or endorsed by Valve.** It presents Valve's USB IDs so Steam recognises it:
for personal use and research on your own hardware, not for sale. Use at your own risk. It has not
been tried on a real puck and is not meant for one.

## What's new since v0.1.1

- **No stuck buttons with current controller firmware.** Steam's controller update of July 2026
  (controller build `0x6A628345`) changed the report a controller sends over the air from `0x45` to
  `0x42`. v0.1.1 only released `0x45` when a link ended, so with an updated controller the PC could
  keep the last buttons and sticks "held" after the controller was switched off. v0.1.2 ends a link
  with a neutral report of whichever kind the controller sent. Checked with a real controller on
  `0x6A628345`: switched off three times with buttons and a trigger held, the PC ended neutral each
  time.
- A controller that was paired with v0.1.1 stays paired: no need to pair again after updating either
  the board or the controller.

## Install

1. Double-tap the board's reset button so a drive appears (or run `bootloader` in its serial shell).
2. Copy `counterpuck-v0.1.2-supermini-nrf52840.uf2` onto it. The board restarts as a puck.
3. In Steam, pair a controller as usual (or run `pair 0` in the board's serial shell).

Check the download against `SHA256SUMS`. Settings (serial numbers, build time reported to Steam,
pairing window, radio power) are compile-time options in `firmware/src/puck_config.h`.

## Known issues

- **Pairing a controller that already has the current firmware may not work.** Analysis of controller
  build `0x6A628345` suggests it no longer takes part in pairing over the air (new pairings seem to
  arrive through the puck's dock contacts instead). Controllers paired before their update keep
  working. Not yet tested.
- After the PC sleeps, the serial shell stays silent until the board is rebooted or replugged. The
  controller link is not affected.
- A rare "no event from the radio" error can appear in the log. It recovers by itself.
- Not tested: a sleep of more than a few seconds; more than one controller at once.
- Steam's own "update" or "downgrade" for a puck is refused (the board's bootloader does not speak
  Steam's update protocol): see `PUCK_STEAM_MAY_REBOOT_TO_BOOTLOADER`.

## Licences

MIT for this project's own code. The image also contains Zephyr (Apache-2.0), nrfx (BSD-3-Clause) and
Nordic's Enhanced ShockBurst library (Nordic 5-clause licence, text in `LICENSE-Nordic-5-Clause.txt`:
use on Nordic chips only). Details: `firmware/THIRD_PARTY.md`.
