# Counterpuck

Notes and tools from taking apart how the **Valve Steam Controller 2 puck** gets its
firmware: the small USB dongle that pairs with, and charges, the 2026 Steam Controller.

The short version: Steam keeps the puck's firmware on your PC, unencrypted, and updates it
with a small Python program over a simple serial protocol. This repo documents where the
files are, what format they're in, and how the update works byte by byte. It includes scripts
so you can check every claim against your own Steam install.

> **Not affiliated with Valve.** Independent interoperability research on hardware I own. No
> Valve firmware, software or captures are included; see [Scope and ethics](#scope-and-ethics).

## Findings

- **The firmware is already on your PC.** Steam keeps it in `Steam\bin\hardwareupdater\` as
  `PROTEUS_FW_<build>.fw`: a 32-byte header (product magic, length, CRC32) on top of a raw,
  unencrypted Cortex-M image. You don't need to read anything off the chip to study it.
- **The puck is a Nordic nRF52833 running Zephyr** (nRF Connect SDK 2.9). Valve's codename
  for it is **Proteus**. The controller is **Triton**, and its radio firmware is **Ibex**.
- **The updater is a bundled Python script.** `hardwareupdater.exe` is PyInstaller around one
  Python 3.14 script using hidapi and pyserial. Its strings lay out the whole update flow.
- **The update was captured live and decoded.** One HID feature report reboots the puck into a
  bootloader that shows up as a USB serial device (`28DE:1007`). The updater then streams the
  image in framed messages. The bytes on the wire are identical to the `.fw` file on disk.
- **No signature is present.** There's none in the file and none on the wire, only a magic
  number, a length and a CRC32. Whether the bootloader checks anything more is untested (I
  won't send modified images to the only puck I have).
- **The HID commands are the original Steam Controller's.** The command IDs and attribute
  numbers match the ones in SDL's open-source Steam Controller code.
- **The app is linked at `0x8000`.** So the bootloader occupies the first 32 KB of flash and
  is not touched by updates.

## Documentation

| Doc | What's in it |
| --- | --- |
| [docs/writeup.md](docs/writeup.md) | The story: how each finding was reached, step by step. Start here |
| [docs/firmware-format.md](docs/firmware-format.md) | Reference: file locations and hashes, the `.fw` header, flash map, build strings |
| [docs/update-protocol.md](docs/update-protocol.md) | Reference: the HID commands and the bootloader serial protocol, byte by byte |
| [docs/reproduce.md](docs/reproduce.md) | How to reproduce every finding from your own Steam install and puck |

## Tools

All in [tools/](tools/). Python 3.11+ using only the standard library (run with `python -I`),
PowerShell for the Windows capture side, and one Ghidra script.

| Tool | Purpose |
| --- | --- |
| `fw_header.py` | Parse a `.fw` header: product magic, length, CRC32, vector table, entropy |
| `find_base.py` | Recover a raw image's flash load address from its own string pointers |
| `pyi_extract.py` | List or extract a PyInstaller bundle without running anything from it |
| `watch_update.ps1` | Passive, non-admin logger: updater launches, USB re-enumeration, new firmware files |
| `capture_usb.ps1` | Capture just the puck's USB traffic with USBPcap (needs admin), leaving your keyboard out |
| `usbpcap_parse.py` | Read USBPcap captures without Wireshark: summary, packet dump, bulk extract, control transfers |
| `decode_bl_stream.py` | Decode the bootloader messages in a capture, rebuild the image and compare it to the `.fw` file |
| `GhidraExport.java` | Ghidra headless post-script that exports decompiled C, a function list and strings |

## Status and next steps

- [x] Locate the firmware in Steam's files and decode the `.fw` container
- [x] Unpack the updater statically
- [x] Capture and decode a live update; confirm the wire image matches the file
- [x] Recover the application's load address
- [ ] Map the puck↔controller radio link (Nordic ESB) and pairing
- [ ] Custom puck hardware: a board in the original's form factor, with charging

[OpenPuck](https://github.com/safijari/openpuck) already makes an off-the-shelf nRF52840 Pro
Micro act as a Steam Controller 2 puck, in software. This project's open-hardware goal builds on
that: a purpose-made board with the dock and charging the original has.

## Scope and ethics

- **Nothing of Valve's is published here.** That includes the `.fw` images, the updater, USB
  captures (they contain the full image) and decompiler output. The docs give file locations
  and SHA-256 hashes instead, and the tools re-create everything locally.
- **Passive observation only.** The update was Steam's own, captured as it happened. No
  modified firmware was sent to the puck, and the bootloader's checks were not probed.
- **No defeating chip protection.** The nRF52833 has debug read-out protection; this project
  won't try to get around it. It doesn't need to, since the application firmware comes from Steam.
- **Clone firmware will be original work**, not Valve's image.

## Licence

Code in `tools/` is [MIT](LICENSE). Documentation (`docs/` and this README) is
[CC BY 4.0](LICENSE-docs).
