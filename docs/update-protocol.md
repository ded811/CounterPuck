# Puck firmware-update protocol

*Reference spec, reconstructed from one live capture (2026-10-09) plus the strings in Steam's
`hardwareupdater.exe` 1.5. Reproduce it from your own puck with [reproduce.md](reproduce.md).*

The update runs in two stages. Stage 1 talks to the running app over HID feature reports and
reboots the puck into its bootloader. Stage 2 streams the image to the bootloader over a USB
CDC serial port. Nothing is encrypted, and the bytes on the wire match the `.fw` file exactly.

## Observed timeline

| t (s) | Event |
| --- | --- |
| 2.0–11.5 | Steam (before the updater): report-1 requests to the docked controller on interface 6, then `0xA3` (read pairing record) on interfaces 5, 2, 4 and 3 |
| 14.0 | Steam / updater enumerate the puck: string descriptors, then HID attribute reads |
| 16.12 | Updater sends **reboot to bootloader** (`0x90`) on interface 5 |
| 16.15 | Puck (`28DE:1304`) drops off USB (hub port-change interrupt) |
| 16.43 | Bootloader `28DE:1007` enumerates (same serial number); Windows' CDC driver configures it by 16.46 |
| 20.14 | Updater opens the serial port (`SET_LINE_CODING`) and sends INFO |
| 20.26 → 30.49 | FW_BEGIN; device ACKs after the **10.2 s erase** |
| 30.54 → 33.49 | 7 × FW_DATA (about 0.5 s per 32 KB chunk) |
| 33.51 | FW_END (header), RESET |
| 35.68 | App re-enumerates as `28DE:1304` |
| 39.8 | Attribute read shows the new build `0x6A628359` |

The bootloader is on the bus 0.3 s after the reboot command, but the updater waits about 3.7 s
before opening it. Erase plus write takes about 13 s, and the app is back 2.2 s after RESET.

## Stage 1: HID feature reports (app mode, `28DE:1304`)

- Command: `SET_REPORT` (Feature), **report ID 2**, 64 bytes: `02 <opcode> <len> <payload…>`, zero-padded.
- Reply: `GET_REPORT` (Feature), report ID 2: `02 <opcode> <len> <payload…>`.
- Report ID 1 is relayed to a controller (the one connected over the radio on interfaces 2–5,
  the docked one on interface 6) and **STALLs** (`USBD_STATUS 0xC0000004`) when no controller is
  behind that interface (see [puck-internals.md](puck-internals.md#usb-device)). A paired but
  unconnected slot stalls too. In this capture no controller was connected over the radio, so
  report 1 stalled on interfaces 2–5, including interface 2, whose slot held a pairing record,
  and the updater used ID 2. A controller was docked, and Steam's report-1 requests on
  interface 6 succeeded: every SET was accepted, and where Steam read the answer (t ≈ 37.2 s and
  41.2 s), its GETs stalled for about 60 ms until the docked controller's reply arrived.
- Sent to interfaces 2–6 (2–5 = radio slots, 6 = dock). The updater's attribute read and reboot
  go to **interface 5**.

The opcodes are the original Steam Controller's HID feature-report commands (Steam's log calls
this "V1 HID protocol via Dongle"). The values match the public SDL source,
[`controller_constants.h`](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/steam/controller_constants.h),
so they aren't secret. Of the commands the updater sent, `0x90` (reboot to bootloader) is the only
one SDL's header doesn't name; Steam itself also sent `0xA3` (read pairing record), which isn't in
the header either.

| Opcode | SDL name | Request payload | Reply payload |
| --- | --- | --- | --- |
| `0xAE` | `ID_GET_STRING_ATTRIBUTE` | `01` = attribute 1 | `01` + ASCII serial (len 0x14) |
| `0x83` | `ID_GET_ATTRIBUTES_VALUES` | none | repeated `tag(u8) value(u32 LE)` records |
| `0x87` | `ID_SET_SETTINGS_VALUES` | `03 09 …` | sent on report ID 1, STALLed here |
| `0x90` | (not in SDL header) | none | reboot to bootloader; device disconnects in about 30 ms |
| `0xA3` | (not in SDL header) | none | 24-byte pairing record of that slot; sent by Steam before the updater ran, and again after the puck came back, not by the updater during the update |
| `0xB4` | `ID_DONGLE_GET_WIRELESS_STATE` | none | 1 byte: 1 = not connected, 2 = connected; seen on interface 6 (the dock), where it means controller undocked / docked |

Attribute tags seen in the `0x83` reply (5 records, len `0x19`). The tag numbers are indices
into SDL's `ControllerAttributes` enum, and all five line up:

| Tag | SDL `ControllerAttributes` | Before update | After update |
| --- | --- | --- | --- |
| `0x01` | `ATTRIB_PRODUCT_ID` | `0x00001304` | same |
| `0x02` | `ATTRIB_CAPABILITIES` | `0x00000000` | same |
| `0x04` | `ATTRIB_FIRMWARE_BUILD_TIME` | `0x6A4423DE` (2026-06-30 20:15:26 UTC) | `0x6A628359` (2026-07-23 21:10:49 UTC) |
| `0x09` | `ATTRIB_BOARD_REVISION` | `0x00000047` | same |
| `0x0A` | `ATTRIB_BOOTLOADER_BUILD_TIME` | `0x68D2F9F2` (2025-09-23 19:50:10 UTC) | same |

Tag `0x04` is what the updater compares against the config to decide whether to update. All
five tags matching SDL's independently maintained enum confirms the puck reuses the original
Steam Controller's attribute numbering.

On report ID 2, the puck's own command channel, the capture shows five opcodes: `0xAE`, `0x83`
and `0x90` from the updater, `0xA3` from Steam before the updater ran, and `0xB4` (with more
`0xA3`) right after the puck came back, sender not attributed. Report-1 requests such as `0x87`
go to a controller, not to the puck. The firmware implements twelve report-2 commands; the full
set, with what each one does, is in
[puck-internals.md](puck-internals.md#vendor-command-channel-feature-report-2).

## Stage 2: bootloader serial protocol (`28DE:1007`, CDC ACM)

The bootloader enumerates as a CDC serial device with the **same USB serial number** as the
puck. The updater finds it with `list_ports` by VID:PID and serial. Data goes out on bulk
EP `0x01` and replies come in on bulk EP `0x82`.

### Framing

```text
0xAD  <msg id>  [payload]  0xAE
```

- Host → device payloads always begin with a constant byte `0x12` (meaning unknown; possibly
  a device-class or protocol byte).
- **Escaping:** inside a frame, `0xAC k` means the byte `0xAC + k`. So `0xAC` is sent as
  `AC 00`, `0xAD` as `AC 01` and `0xAE` as `AC 02`. Verified: un-escaping all FW_DATA frames
  reproduces the `.fw` payload bit-for-bit.
- Device → host replies: `AD <status> [payload] AE`. `status 0x00` = ACK. Every host
  message got exactly one reply before the next was sent.

### Messages

| ID | Name (updater constant) | Host payload after `0x12` | Device reply |
| --- | --- | --- | --- |
| `0x33` | `MESSAGE_INFO` | none | ACK + 164-byte info block (below) |
| `0x34` | `MESSAGE_FW_BEGIN` | none | ACK after the erase completes (10.2 s) |
| `0x35` | `MESSAGE_FW_DATA` | `u16 LE length` + data (`0x8000` per chunk observed) | ACK |
| `0x36` | `MESSAGE_FW_END` | the 32-byte `.fw` header (magic, length, CRC32, zeros) | ACK |
| `0x37` | `MESSAGE_RESET` | none | ACK, then the device reboots into the app |

`MESSAGE_PROVISION` (factory serial-number write) exists in the updater but was not sent.

FW_DATA carries the image payload only (the `.fw` file minus its 32-byte header), in order,
with no address field. In this capture that was 6 × 32768 + 1368 = 197,976 bytes. The header
goes last, in FW_END. That suggests the bootloader checks length and CRC32 over what it
received before marking the image valid; this is inferred, not observed.

### INFO reply block (164 bytes after the status byte)

| Offset | Size | Value (this puck) | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `0x68D2F9F2` | Bootloader build time (2025-09-23), same as HID tag `0x0A` |
| 4 | 32 | magic `0x2E795631`, len `0x3047C` (197,756), CRC32 `0x9359B95E`, zeros | Header of the **currently installed** app image |
| 36 | 4 | `0xAC32A429` | Provisioning magic (`PROVISIONING_MAGIC`) |
| 40 | 4 | `0x00000047` | Hardware ID (same value as HID tag `0x09`, the board revision) |
| 44 | 16 | `FXBxxxxxxxxxx` | Unit serial (also the USB serial) |
| 60 | 16 | `MXAxxxxxxxxxx` | PCBA serial |
| 76 | 88 | zeros | unused |

Bytes 36–75 match the updater's `struct.unpack('<II16s16s', …)`.

## Open questions

- What the constant `0x12` after each host message ID means.
- Whether the bootloader rejects a bad CRC32 or magic. Untested: I won't send modified images
  to the only puck I have.
- Whether the bootloader verifies anything else, such as a signature stored outside the
  `.fw` file. No sign of one on the wire.
