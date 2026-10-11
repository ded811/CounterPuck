# Firmware files and the `.fw` container

Steam ships the puck's firmware in its install folder as an unencrypted Cortex-M image with
a 32-byte header. This page covers where the files are, what the header holds, how the image
is laid out in flash, and what the build strings reveal.

Everything here was checked against Steam's files as of October 2026 (puck build `0x6A628359`)
on Windows. Valve's files are not redistributed in this repository. Use the hashes below to
confirm you're looking at the same build.

## Where the files are

```text
<Steam install>\bin\hardwareupdater\
    hardwareupdater.exe        the updater Steam runs
    hardwareupdater.cfg        which builds are current, and the forced-update floor
    PROTEUS_FW_<build>.fw      puck firmware
    IBEX_FW_<build>.fw         controller firmware
```

On a default Windows install that's `C:\Program Files (x86)\Steam\bin\hardwareupdater\`.
I haven't checked the Linux or macOS clients.

| File | Size (bytes) | SHA-256 |
| --- | --- | --- |
| `PROTEUS_FW_6A628359.fw` | 198,008 | `b84853fec0d40c9dfe7948a4d37cbbf17e58f60962897f3ba5bda867253a6934` |
| `IBEX_FW_6A628345.fw` | 384,476 | `865b4a7b1786c4a9990375759548331d313170c3e3b4c5897e16dc01b9ab12a8` |
| `hardwareupdater.exe` | 8,718,056 | `af546b4d2440c6133f78d3bacebc304cd73531ea8806fc4fe996d165d8929722` |
| `hardwareupdater.cfg` | 116 | `fbfb0054c720c19b3ba6d8843d0af9fb5c1202f78ca32b48affcbfc42c3e0924` |

## Codenames

| Codename | What it is | Evidence |
| --- | --- | --- |
| **Proteus** | The puck (USB dongle and charging dock) | Boot banner `Starting Proteus BUILD_TIME_%08x GIT_SHA_%s`; strings `hid-puck`, `udc_nrfx`, `esb/ibex_0..3`; contains the `28DE:1304` USB device descriptor |
| **Ibex** | The controller's radio firmware | Boot banner `Starting Triton BUILD_TIME_...`; strings `ST_PUCK_KEYCHORD_ESB_entry`, `Connecting to: Proteus %s` |
| **Triton** | The controller as a product | Steam's controller log (`CTritonDockedToPuckWorkItem`); the config key `TRITON_FW_TS` names the IBEX file |
| **Nereid** | Board revision `0x44` of the puck firmware (USB PID `0x1305`); the product itself is unknown | The updater knows a device type `Nereid USB` and groups it with Proteus as a "dongle"; the puck firmware logs `Failed to set Nereid USB PID` and uses PID `0x1305` on board revision `0x44` ([puck-internals.md](puck-internals.md#nereid)) |

## Versions are build timestamps

There are no version numbers. Every build is identified by its Unix build time, written in hex
in the filename, in the config, and in the device's own attributes.

`hardwareupdater.cfg`:

```text
MUST_UPDATE_TRITON_FW_TS:6A18D057     2026-05-28 23:31:35 UTC
MUST_UPDATE_PROTEUS_FW_TS:6A18D053    2026-05-28 23:31:31 UTC
TRITON_FW_TS:6A628345                 2026-07-23 21:10:29 UTC  (current controller build)
PROTEUS_FW_TS:6A628359                2026-07-23 21:10:49 UTC  (current puck build)
```

Going by the key names and the updater's `must_update` / `updates_available` fields, a device
older than the current build is offered an update, and one older than the `MUST_UPDATE` floor
is forced to take it. (The dates on the right are my annotations, not part of the file.)

## Container header

All fields are 32-bit little-endian.

| Offset | Field | PROTEUS | IBEX | Notes |
| --- | --- | --- | --- | --- |
| `0x00` | Product magic | `0x2E795631` (ASCII `1Vy.`) | `0xD2D86467` | Hard-coded in the updater as `PROTEUS_FIRMWARE_HEADER_MAGIC` / `TRITON_FIRMWARE_HEADER_MAGIC` and checked before flashing |
| `0x04` | Payload length | `0x00030558` (197,976) | `0x0005DDBC` (384,444) | Always file size − 32 |
| `0x08` | Payload CRC32 | `0x747A4771` | `0x0009E42D` | Standard CRC-32 (zlib) over the payload only |
| `0x0C` | Reserved | 0 | 0 | 20 bytes of zeros |
| `0x20` | Payload | | | Raw image, written to flash as-is |

There's no signature, key ID or encryption field in the container. During an update the
payload goes to the bootloader first and this 32-byte header goes last, unchanged (see
[update-protocol.md](update-protocol.md)). The bootloader later reports the stored header back,
so it evidently keeps a copy.

## The payload is a plain Cortex-M image

The payload starts with a normal ARMv7-M vector table:

| | PROTEUS (puck) | IBEX (controller) |
| --- | --- | --- |
| Initial SP | `0x20015D00` | `0x20016D80` |
| Reset handler | `0x00011EAD` | `0x00026B71` |
| Handler range | `0x11E99`–`0x25939` | `0x26B5D`–`0x45EBD` |
| Mean entropy | 5.38 bits/byte | 6.49 bits/byte |

Entropy that low, with a readable vector table and hundreds of plain-text strings, means the
image is neither encrypted nor compressed.

### Load address and flash map (puck)

The PROTEUS image is linked at **`0x00008000`**. I found this by counting how many 32-bit
words in the image point exactly at the start of one of its own strings, for every candidate
base on a 256-byte grid. `0x8000` scored 491 hits and the runner-up 44
([`tools/find_base.py`](../tools/find_base.py)).

| Range | Contents | Source |
| --- | --- | --- |
| `0x00000`–`0x07FFF` | Bootloader (32 KB), flash area 0 | Not in any Steam file; size from the app's flash map |
| `0x08000`–`0x7CFFF` | Application partition (0x75000), flash area 1; the payload (197,976 bytes) fills `0x08000`–`0x38557` | `.fw` payload; flash map |
| `0x7D000`–`0x7FFFF` | Settings / NVS (12 KB), flash area 2: pairing records, reset-reason record | The app's flash map |
| `0x20000000`–`0x2001FBFF` | RAM used by the app (127 KB); stack top at `0x20015D00` | Vector table; MPU setup |
| `0x2001FC00`–`0x2001FEFF` | Not used by the app | App's RAM end |
| `0x2001FF00`–`0x2001FFFF` | No-init RAM shared with the bootloader (reboot request, crash record) | App code |

The flash map comes from the app itself: Zephyr's fixed flash-area table in the image lists exactly
these three areas on the internal flash controller. Where the bootloader keeps the stored `.fw`
header it reports back during an update is still unknown.

### App header (payload `+0x100`)

Right after the 64-entry vector table, at payload offset `0x100` (flash `0x8100`), sits a 40-byte
header. The controller's IBEX image has the same header at the same offset, with the same
constants and git hash and only its own build time.

| Offset | Size | PROTEUS value | Meaning |
| --- | --- | --- | --- |
| `+0x00` | 8 | `0x7EA46046`, `0xB9863E5A` | Magic words |
| `+0x08` | 4 | `0x000D1805` | Unknown (same in IBEX) |
| `+0x0C` | 16 | `fec01234c8af` + NULs | Git short hash of the build |
| `+0x1C` | 4 | `06 0A 04 00` | Unknown, possibly a version (same in IBEX) |
| `+0x20` | 4 | `0x6A628359` | Build time (Unix seconds), the same value as the file name and HID attribute 4 |
| `+0x24` | 4 | `FF FF 00 00` | Unknown (same in IBEX) |

The app itself only reads the git hash and the build time from it: for HID attribute 4, string
attribute 3, the device-info reply of HID command `0xF2`, the boot banner, and the crash
(reset-reason) record. In PROTEUS, code starts at payload offset `0x140` (the 24 bytes after the
header are `0xFF` padding); in IBEX it follows the header directly, at payload offset `0x128`.

The nRF52833 has 512 KB of flash (`0x00000`–`0x7FFFF`) and 128 KB of RAM.

## Build environment

Strings in both images give the toolchain away:

- `*** Using nRF Connect SDK v2.9.0-d93dcad627bd ***`
- `*** Using Zephyr OS v3.7.99-21bb7021858d ***`
- in PROTEUS only, newlib for `arm-zephyr-eabi` (the full library, not newlib-nano). The source
  paths embedded in it (`/__w/_temp/workspace/build/.build/…`) come from the prebuilt Zephyr SDK
  toolchain's own build, not from Valve's build machines. They are the paths of the toolchain's
  Linux build (its Windows build has an extra `HOST-x86_64-w64-mingw32` component there), so
  Valve used the Linux-hosted Zephyr SDK: the firmware was most likely (*inferred*) built on
  Linux or under WSL.

Neither commit in those banners exists in Nordic's public repositories: the official v2.9.0
release is sdk-nrf `7787b2649840` and sdk-zephyr `1f8f3dc29142`. Valve builds from its own forks
of both. The differences that show in the image (changes to the USB stack and its HID class, two
backported upstream fixes, a kernel device-state bit, extra sensor channels, and full newlib where
NCS 2.9 defaults to picolibc) are listed in
[puck-internals.md](puck-internals.md#software-stack), along with the build configuration.

The puck firmware stores one pairing record per radio slot under the Zephyr settings keys
`esb/ibex_0`..`esb/ibex_3`: 24 bytes each, `{proteus_uuid, ibex_uuid, controller serial}` (the
strings `ibex%d_proteus_uuid`, `ibex%d_ibex_uuid` and `ibex%d_peer_sn` are output formats for
printing them). So pairing data lives in the settings partition on the chip, not in the `.fw`
file.

## What the `.fw` file does not contain

- The **bootloader** (flash `0x0000`–`0x7FFF`). The puck's bootloader reported a build time of
  2025-09-23 and was not changed by the update.
- **UICR** (Nordic's user configuration registers, including debug-port protection settings; see
  [puck-internals.md](puck-internals.md#debug-port)).
- The **settings/pairing partition** and the per-unit serial numbers.

## Check it yourself

```text
python -I tools/fw_header.py  "<Steam>\bin\hardwareupdater\PROTEUS_FW_6A628359.fw"
python -I tools/find_base.py  "<Steam>\bin\hardwareupdater\PROTEUS_FW_6A628359.fw"
```

See [reproduce.md](reproduce.md) for the full walkthrough.
