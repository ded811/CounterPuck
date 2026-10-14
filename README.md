# Counterpuck

An open firmware that makes a cheap **nRF52840 dev board** behave like a **Valve Steam Controller 2
puck**, and the research notes about the real puck that it is built from.

> **Unofficial.** Not made, endorsed or supported by Valve. Independent interoperability work on
> hardware I own. No Valve firmware, software or captures are included; see
> [Scope and ethics](#scope-and-ethics). The firmware presents Valve's USB IDs so that Steam
> recognises it, so it is for personal use and research, not for sale.

| | |
| --- | --- |
| **[Firmware](firmware/README.md)** | An original, MIT-licensed puck for a SuperMini / Pro Micro nRF52840. Plug it in, Steam sees a puck. |
| **[Research](#what-we-found)** | How the real puck works and how Steam updates it, with the documents and scripts to check every claim on your own PC. |

## Try the firmware

1. Download `counterpuck-…-supermini-nrf52840.uf2` from the
   [latest release](https://github.com/ded811/Counterpuck/releases/latest).
2. Double-tap the board's reset button so a drive appears, and copy the file onto it.
3. In Steam, pair a controller as usual.

A real Steam Controller 2 pairs with it, connects, plays, rumbles, and is switched off and woken with
the PC's sleep. It is written from observed behaviour, with no Valve code, on the stock nRF Connect
SDK. **Status: v0.1.2, tested with one controller.** What was verified, the known issues and the
settings are in the [firmware README](firmware/README.md).

Not built yet: charging and the pogo-pad dock, and custom hardware in the original's shape.

## What we found

The headlines, from taking apart Steam's own files and capturing a real update. The story of how
each was reached is in [docs/writeup.md](docs/writeup.md).

- **The firmware is already on your PC.** Steam keeps it, unencrypted, in
  `Steam\bin\hardwareupdater\PROTEUS_FW_<build>.fw`: a 32-byte header (magic, length, CRC32) on a raw
  Cortex-M image. Nothing has to be read off the chip.
- **The puck is a Nordic nRF52833 running Zephyr** (nRF Connect SDK 2.9, from Valve's own forks).
  Valve's codenames: **Proteus** is the puck, **Triton** the controller, **Ibex** its radio firmware.
- **The update protocol is simple and open.** One HID feature report reboots the puck into a
  bootloader (`28DE:1007`, a USB serial port); the updater then streams the image in framed
  messages. The wire bytes equal the `.fw` file. There is no signature, only a magic number, a length
  and a CRC32. (Whether the bootloader checks more is untested: I won't send modified images to
  the only puck I have.)
- **The controller works the same way.** Its firmware (`IBEX_FW_<build>.fw`) sits next to the
  puck's, in the same container, unencrypted. It is updated only over its USB cable, through its
  own bootloader `28DE:1005`. Since its July 2026 update it sends its state to the puck as report
  `0x42`. How it works inside, and a rebuild of it, are in the
  [research repository](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/README.md).
- **The updater is a bundled Python script** (`hardwareupdater.exe`: PyInstaller, Python 3.14,
  hidapi, pyserial). Its strings lay out the whole update flow.
- **The radio link and the vendor commands are documented**: 12 HID commands (pairing is `0xAD`),
  Nordic ESB at 2 Mbps with the base address `"ibex"`, the pairing and connection sequence, channel
  choice, link-quality reports, the dock, the pin map and power setup a compatible board needs.
  The command IDs match the original Steam Controller's, as in SDL's open-source driver.
- **Valve's SDK changes can be read from the machine code**: a reworked USB HID class, quieter USB
  logs, a shell lock timeout and a few more. Two are upstream Zephyr fixes Valve backported.
- **The debug port is as the factory left it.** The application neither locks nor opens it, so
  whether it is open depends on how the chip was programmed. Only a read-only probe can tell
  ([details](docs/puck-internals.md#debug-port)).

## Documentation

| Doc | What's in it |
| --- | --- |
| [docs/writeup.md](docs/writeup.md) | The story of how each finding was reached. Start here |
| [docs/firmware-format.md](docs/firmware-format.md) | The `.fw` file: locations, hashes, header, flash map, build strings |
| [docs/update-protocol.md](docs/update-protocol.md) | The HID commands and the bootloader serial protocol, byte by byte |
| [docs/puck-internals.md](docs/puck-internals.md) | What the firmware does: SDK fork, USB layout, commands, radio link, pairing, dock, pins, power |
| [docs/wire-formats.md](docs/wire-formats.md) | The report and packet formats the open firmware is written from |
| [docs/reproduce.md](docs/reproduce.md) | How to reproduce every finding from your own Steam install and puck |

## Tools

In [tools/](tools/): Python 3.11+ with only the standard library (run with `python -I`), and
PowerShell for the Windows capture side.

| Tool | Purpose |
| --- | --- |
| `fw_header.py` | Parse a `.fw` header: magic, length, CRC32, vector table, entropy |
| `find_base.py` | Recover a raw image's load address from its own string pointers |
| `watch_update.ps1` | Passive logger of updater launches, USB re-enumeration and new firmware files |
| `capture_usb.ps1` | Capture just the puck's USB traffic with USBPcap (admin needed) |
| `usbpcap_parse.py` | Read USBPcap captures without Wireshark |
| `decode_bl_stream.py` | Decode the bootloader messages in a capture and rebuild the image |

The tools that read Valve's code (Ghidra scripts, the PyInstaller extractor) live in the sister
repository, [steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered),
next to a study rebuild of Valve's own firmware. Nothing here is derived from that code.

## Roadmap

- [x] Locate the firmware in Steam's files; decode the `.fw` container and the update protocol
- [x] Capture a live update and confirm the wire image matches the file
- [x] Map the radio link, pairing and the dock; pin down Valve's SDK changes
- [x] Open firmware for an nRF52840 dev board, checked against a real controller (v0.1.0)
- [ ] Check the debug port, read-only: open or locked? (needs opening the puck, so it waits for the
      hardware work)
- [ ] Custom hardware: a board in the original's shape, with the dock and charging

Related: [OpenPuck](https://github.com/safijari/openpuck) also turns an nRF52840 into a puck, with
extra controller modes, built from black-box radio analysis. This project's hardware goal is a
purpose-made board with the dock and charging the original has.

## Scope and ethics

- **Nothing of Valve's is published here**: no `.fw` images, no updater, no USB captures (they
  contain the full image), no decompiler output. The docs give file locations and SHA-256 hashes
  instead, and the tools re-create everything locally.
- **Passive observation.** The update captured was Steam's own. No modified firmware was sent to
  the puck, and the bootloader's checks were not probed.
- **No defeating chip protection.** The nRF52833 has debug read-out protection; this project won't
  try to get around it, and doesn't need to.
- **The firmware is original work**, written from observed behaviour and documented in
  [docs/wire-formats.md](docs/wire-formats.md). It is a specification-driven rewrite rather than a
  strict clean room, as its README says.

## Licence

Code in `tools/` and the firmware in `firmware/` are [MIT](LICENSE); a built firmware image also
contains SDK code under its own licences ([firmware/THIRD_PARTY.md](firmware/THIRD_PARTY.md)).
Documentation (`docs/` and this README) is [CC BY 4.0](LICENSE-docs).
