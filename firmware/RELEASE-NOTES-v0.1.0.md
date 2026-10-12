# Counterpuck v0.1.0

An open firmware that makes an **nRF52840 dev board** (SuperMini / Pro Micro with the Adafruit UF2
bootloader) behave like a Steam Controller 2 puck. First release.

**Unofficial, not made or endorsed by Valve.** It presents Valve's USB IDs so Steam recognises it:
for personal use and research on your own hardware, not for sale. Use at your own risk. It has not
been tried on a real puck and is not meant for one.

## Install

1. Double-tap the board's reset button so a drive appears.
2. Copy `counterpuck-v0.1.0-supermini-nrf52840.uf2` onto it. The board restarts as a puck.
3. In Steam, pair a controller as usual (or run `pair 0` in the board's serial shell).

Check the download: `SHA256SUMS` lists the checksum. Settings (serial numbers, build time reported to
Steam, pairing window, radio power) are compile-time options in `firmware/src/puck_config.h`.

## What works (tested with a real Steam Controller 2)

- the USB device a puck presents, and every vendor command Steam sends it;
- pairing, connecting, protocol version 1, channel switching, link quality reports;
- input at about 270 reports per second, buttons, sticks and trackpads through the combined report;
- rumble (Steam's output reports reach the controller and it vibrates);
- a background noise scan that steers channel choice;
- PC sleep and wake: the controller is switched off when the PC sleeps and reconnects when it wakes;
- pairings kept in flash across reflashing.

## Known issues

- After the PC sleeps, the serial shell stays silent until the board is rebooted or replugged. The
  controller link is not affected.
- A rare "no event from the radio" error can appear in the log while the puck is searching for a
  controller. It recovers by itself and has no visible effect; its cause is not found.
- Not tested: a sleep of more than a few seconds; more than one controller at once.
- Steam's own "update" or "downgrade" for a puck is refused (the board's bootloader does not speak
  Steam's update protocol): see `PUCK_STEAM_MAY_REBOOT_TO_BOOTLOADER`.

## Licences

MIT for this project's own code. The image also contains Zephyr (Apache-2.0), nrfx (BSD-3-Clause) and
Nordic's Enhanced ShockBurst library (Nordic 5-clause licence, text in `LICENSE-Nordic-5-Clause.txt`:
use on Nordic chips only). Details: `firmware/THIRD_PARTY.md`.
