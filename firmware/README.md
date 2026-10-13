# Counterpuck firmware

An open firmware that makes a **Nordic nRF52840 dev board** behave like a Valve Steam Controller 2
puck, as close as we can get from outside. A stock, vanilla experience: plug it in, Steam sees a
puck, and your controller pairs and plays.

> **Status: first working version (2026-10-11).** It builds on the stock SDK and runs on a
> SuperMini nRF52840. A PC sees the same USB device as a puck, answers every vendor command, and
> a real Steam Controller 2 pairs, connects on protocol 1 and sends input at about 250 reports per
> second. The button values match the reference build. The background noise scan runs (eight
> sweeps a second, no effect on the link). Verified with a real controller: rumble works
> (Steam sends output reports 0x81, 0x82 and 0x85 and the controller vibrates), and PC sleep and wake work (the controller is
> switched off when the PC sleeps and reconnects when it wakes). Open: a longer sleep test, and a
> rare "no event from the radio" error. It shows up now and then while the puck is searching for a
> controller, mostly just after a start; the radio code recovers from it and the log records the
> packet it happened on, but the cause is not found.

Build: `.\build.ps1` (needs a stock nRF Connect SDK v2.9.0). Flash: copy `zephyr.uf2` onto the
board's drive, or run `bootloader` in the serial shell first.

This is a different project from [steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered),
which rebuilds Valve's own firmware for study. Nothing here is derived from that code.

## Before you use it

- **Unofficial.** Not made, endorsed or supported by Valve. Steam, Steam Controller and the Valve
  logo are trademarks of Valve Corporation. The firmware is written from observed behaviour and
  contains no Valve code.
- **It uses Valve's USB IDs.** Steam finds a puck by its vendor and product ID (`28DE:1304`), so
  the board has to present them to work with Steam at all. Those IDs belong to Valve. This is meant
  for personal use and research on your own hardware: do not sell boards that present them, and
  expect Windows and Steam to treat the board as a puck.
- **At your own risk.** It runs on a dev board you can reflash at any time. It has not been tried on
  a real puck, and nothing here is meant for one.
- **Licences.** MIT for what is written here; the image also contains Zephyr, Nordic's ESB library
  and other SDK code under their own licences ([THIRD_PARTY.md](THIRD_PARTY.md)).

## Known issue

After the PC has slept and woken, the serial shell stays silent until the board is rebooted or
replugged. The USB device and the controller link are unaffected: after two short PC sleeps the
controller was still connected and sending about 250 reports per second. Valve's puck ships
changes to Zephyr's shell and serial class that look aimed at this; this firmware uses the stock
ones.

## Using it

1. **Flash.** Put the board in bootloader mode (double-tap its reset button) and copy `zephyr.uf2` onto
   the drive that appears. To build it yourself: install nRF Connect SDK v2.9.0 (unpatched) and run
   `.uild.ps1`. Once it runs, `bootloader` in the serial shell (below) reboots into the drive mode
   without the button.
2. **Plug it in.** The PC sees the same device as a Steam Controller 2 puck: Steam lists it, and the
   board's USB serial port carries a shell.
3. **Pair.** In Steam, add a controller as usual and put the controller into pairing mode. Or, without
   Steam, run `pair 0` in the shell (a 60 s window on slot 0) and start pairing on the controller.
   Pairings are kept in flash and survive reflashing. Not yet tested: pairing a controller that already
   runs the July 2026 controller firmware (`0x6A628345`), which may no longer pair over the air.
   Controllers paired before that update keep working.
4. **Settings.** Serial numbers, USB strings, the build time reported to Steam, the pairing window and
   the radio power are in [`src/puck_config.h`](src/puck_config.h), each with a comment saying what a
   real puck uses.

Shell commands (serial port, any baud rate, prompt `dongle~$ `):

| Command | What it does |
| --- | --- |
| `info` | serial numbers, revision, build time |
| `bonds` | the four pairing records |
| `connection` | each slot's state; channels and uptime when connected |
| `pair <slot> [seconds]` | open a pairing window |
| `connection_qos [ms] [1]` | change how often link quality is reported; `1` also prints it |
| `bootloader` | reboot into the UF2 drive mode |
| `trace_output [off]` | log every output report (rumble, LEDs) the PC sends to a controller |
| `factory_restore --really` | forget every pairing and reboot |

Steam's own "update" or "downgrade" for a puck asks the board to reboot into its bootloader. This
board's bootloader does not speak Steam's update protocol, so the request is refused and logged by
default (`PUCK_STEAM_MAY_REBOOT_TO_BOOTLOADER` in the config header). Steam also sees the board as
up to date, because it reports a newer build time than Valve's current puck.

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
