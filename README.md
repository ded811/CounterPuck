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
- **The puck is a Nordic nRF52833 running Zephyr** (nRF Connect SDK 2.9, built from Valve's own
  fork). Valve's codename for it is **Proteus**. The controller is **Triton**, and its radio
  firmware is **Ibex**.
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
- **The firmware is Zephyr all the way down**, and static analysis of it yields the rest of the
  picture ([docs/puck-internals.md](docs/puck-internals.md)): the full 12-command vendor HID
  set (pairing is `0xAD`, pairing records are read/written with `0xA3`/`0xA2`), the
  app↔bootloader handshake through a RAM mailbox, the radio configuration (ESB, 2 Mbps BLE
  PHY, base address `"ibex"`, even channels 2–80: pairing and connection requests on 2, data on
  4–78), the pairing and connection sequence, how the dock works (the `P` pad carries an analog
  presence signal and, *inferred* from the firmware, a half-duplex UART on the same net), and
  the pin map, power setup and memory split a compatible board needs.
- **Valve's SDK changes can be read from the machine code.** Compiling the stock NCS 2.9.0
  source with a candidate change and comparing the result with the image function by function
  pins down each difference byte for byte: a reworked USB HID class (copied reports,
  suspend/resume callbacks), a USB driver that halts on a refused endpoint event, separate
  CDC-ACM receive and transmit buffers, a kernel "device disabled" bit, two extra sensor
  channels and quieter USB logging. Two of the changes are later upstream Zephyr fixes that
  Valve backported: 13 of the 22 silenced USB log lines, and a shell lock timeout.
- **The app leaves the debug port as the factory set it.** Its startup code copies
  `UICR.APPROTECT` and never forces protection on, so whether the port is open depends on how
  the chip was programmed, and on the bootloader, which isn't in any Steam file. Only a
  read-only probe can tell
  ([docs/puck-internals.md](docs/puck-internals.md#debug-port)).

## Documentation

| Doc | What's in it |
| --- | --- |
| [docs/writeup.md](docs/writeup.md) | The story: how each finding was reached, step by step. Start here |
| [docs/firmware-format.md](docs/firmware-format.md) | Reference: file locations and hashes, the `.fw` header, flash map, build strings |
| [docs/update-protocol.md](docs/update-protocol.md) | Reference: the HID commands and the bootloader serial protocol, byte by byte |
| [docs/puck-internals.md](docs/puck-internals.md) | Reference: what the firmware does — SDK fork, USB layout, vendor commands, radio link, pairing, dock, pins and power, debug port, Nereid |
| [docs/reproduce.md](docs/reproduce.md) | How to reproduce every finding from your own Steam install and puck |

## Tools

All in [tools/](tools/). Python 3.11+ using only the standard library (run with `python -I`),
PowerShell for the Windows capture side, and four Ghidra scripts (Java, for Ghidra's headless
analyzer).

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
| `GhidraXrefDump.java` | Writes each function's string and data references, the call graph, and every pointer stored in data (for decoding Zephyr's static tables) to `xrefs.tsv` |
| `GhidraCreateFunctions.java` | Creates functions at addresses listed in a text file (for example handlers found in Zephyr's tables that auto-analysis missed), then re-runs analysis |
| `GhidraApplyNames.java` | Applies a JSON naming table of your own (functions, labels, Zephyr `struct device`s), marks flash read-only and types literal-pool pointers so the decompiler shows constants, then re-exports |

## Status and next steps

- [x] Locate the firmware in Steam's files and decode the `.fw` container
- [x] Unpack the updater statically
- [x] Capture and decode a live update; confirm the wire image matches the file
- [x] Recover the application's load address
- [x] Map the puck↔controller radio link (Nordic ESB), pairing and the dock behaviour
- [x] Pin down what Valve changed in its SDK fork, from the machine code
- [ ] Confirm the radio findings against a real controller
- [ ] Check the debug port read-only: open or locked?
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
