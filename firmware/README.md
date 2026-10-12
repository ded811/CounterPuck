# Counterpuck firmware

An open firmware that makes a **Nordic nRF52840 dev board** behave like a Valve Steam Controller 2
puck, as close as we can get from outside. A stock, vanilla experience: plug it in, Steam sees a
puck, and your controller pairs and plays.

> **Status: planning.** No firmware code yet. This page fixes the scope so the code can follow it.

This is a different project from [steam-puck-reverse-engineered](https://github.com/ded811/steam-puck-reverse-engineered),
which rebuilds Valve's own firmware for study. Nothing here is derived from that code.

## Goal: behave the same, not be the same

What matters is what a PC and a controller can observe:

- the USB device (IDs, strings, interfaces, report formats) and the vendor commands Steam sends;
- the radio exchange with a controller: pairing, connecting, input, feature reports, link
  quality, channel changes;
- timing that either side depends on.

The code, structure and names are our own. Where a behaviour is not documented, we test it against
a real controller and write down what we see.

## Scope

| In | Out, for now |
| --- | --- |
| USB: serial port, 4 slot HID interfaces, 1 dock HID interface (present, idle) | Charging and the pogo dock (the dev board has no pogo pads) |
| Vendor commands: attributes, serials, pairing, bonds, reboot, device info, channel stats | The Nereid I2C personality |
| Radio: pairing, connecting, input, feature and output reports, channel choice | The retail puck board and Valve's bootloader |
| Pairing records kept in flash | Anything that needs a custom board |
| Shell with a few diagnostic commands | |

Custom hardware (a board shaped like the original, with charging) is a later project. This
firmware is written so that part can be added without rewriting the rest.

## Platform

- **Stock nRF Connect SDK v2.9.0**, with Zephyr's own USB stack and HID class, Nordic's ESB
  library, and no patches to the SDK.
- Board: SuperMini / Pro Micro nRF52840 with the Adafruit UF2 bootloader. A small board
  definition ships here, adapted from Zephyr's (Apache-2.0, headers kept).
- Licence: MIT for everything we write.

## Configuration

Everything you might want to change lives in one header, `src/puck_config.h`, with a comment on
each setting saying what it is for and what a real puck uses: the serial numbers, the USB
strings, the build identity, the pairing window and the radio settings.

## What it is written from

Behaviour only: the documents in this repository and in the research repository, and black-box
tests against a real controller. No source code, addresses or decompiler output were used to write
it. This is a specification-driven rewrite, not a strict clean room: the person who wrote the
specifications also wrote this firmware.
