# Wire formats

The byte-level formats a puck and a controller exchange, collected as one reference. It
complements [puck-internals.md](puck-internals.md), which describes the behaviour in prose, and is
the specification the open firmware in [firmware/](../firmware/README.md) is written from. Every
format here is something a PC or a controller can observe; each was checked against a real
Steam Controller 2 on 2026-10-11.

## USB HID reports

Each slot interface (`HID_0`..`HID_3`, interfaces 2 to 5) carries three top-level collections.
Report sizes below exclude the report ID byte.

| ID | Collection | Direction | Bytes | Content |
| --- | --- | --- | --- | --- |
| `0x40` | mouse (generic desktop 0x02) | in | 5 | buttons 1 and 2 (bits 0 and 1, six padding bits), X, Y, wheel, horizontal pan; each axis a signed byte, -127..127 |
| `0x41` | keyboard (generic desktop 0x06) | in | 8 | the boot keyboard layout: modifier bits E0..E7, a reserved byte, six key codes 0..0x65 |
| `0x42` | vendor page `0xFF00`, usage `0x01` | in | 53 | controller state |
| `0x43` | same | in | 14 | passed through from the controller |
| `0x44` | same | in | 5 | passed through from the controller |
| `0x45` | same | in | 45 | controller state, see [the combined report](#the-combined-report-0x45) |
| `0x79` | same | in | 1 | link status |
| `0x7B` | same | in | 12 | link quality |
| `0x80`..`0x89` | same | out | 9, 7, 3, 9, 8, 3, 3, 63, 63, 63 | output reports for the controller, in ID order |
| `0x01` | same | feature | 63 | passed through to and from the controller |
| `0x02` | same | feature | 63 | vendor commands for the puck itself |

In the vendor collection each report's HID usage equals its ID (feature `0x02` also uses usage
`0x01`), every field is 8 bits wide and spans 0..255. The dock interface (`hid-puck`, interface 6)
is one collection with vendor page `0xFF00`, usage `0x02`, holding input `0x42` (53), input `0x79`
(1) and features `0x01` and `0x02` (63 each). All five interfaces have an interrupt IN and an
interrupt OUT endpoint of 64 bytes; the slots poll every 2 ms and the dock every 32 ms.

`SET_REPORT` and `GET_REPORT` for features carry the report ID as the first byte and are 64 bytes
long in all.

## The combined report 0x45

Some controllers send their state as two radio records, `0x60` and `0x61`, 20 bytes each. The
puck merges the latest of each into one `0x45` report for the PC.

Each half: `{id, buttons u16, axis u8, v[8] int16}`, little endian, 20 bytes.

The `0x45` report, 46 bytes with its ID, little endian:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 1 | `0x45` |
| 1 | 1 | sequence number, +1 for every report sent |
| 2 | 4 | button field |
| 6 | 2 | axis from `0x60`, the byte widened to 15 bits |
| 8 | 2 | axis from `0x61`, widened the same way |
| 10 | 4 | `0x60` v[0], v[1] |
| 14 | 4 | `0x61` v[0], v[1] |
| 18 | 16 | zeros, never written |
| 34 | 12 | `0x60` v[2]..v[7] |

Widening a byte `b` to 15 bits: `(b << 7) | (b & 0x80 ? 0x7F : 0)`.

Button field: for every set bit *i* of a half's 12 used button bits, set the destination bit; for
every clear bit, clear it (each half only touches its own destination bits). Source bit to
destination bit:

| Half | Source -> destination |
| --- | --- |
| `0x60` | 0->10, 1->11, 2->12, 3->13, 9->14, 6->15, 8->16, 4->17, 5->19, 10->24, 7->27, 11->29 |
| `0x61` | 0->0, 1->1, 2->2, 3->3, 8->4, 6->5, 9->6, 4->7, 5->9, 10->20, 7->23, 11->28 |

Sending: one `0x45` after every record received, the one on the lowest-numbered connected slot that
has sent a half-report. With nothing received for a second, a neutral report (everything but ID and
sequence zero) is sent. When that controller's link ends, a neutral report goes to its interface too, so
the PC is not left holding its last buttons and sticks. The same neutral report ends a link whose
controller sent finished `0x45` reports itself (as the controller used for testing does).

## Link status 0x79 and link quality 0x7B

`0x79` on a slot: 1 no link, 2 connected, 3 just paired. On the dock interface: 2 docked, 1 not.

`0x7B`, little endian after the ID:

| Offset | Size | Field |
| --- | --- | --- |
| 1 | 2 | window length, ms |
| 3 | 2 | successful exchanges |
| 5 | 2 | retries (misses) |
| 7 | 2 | longest gap between acknowledgements, ms |
| 9 | 1 | mean RSSI of the acknowledgements, dBm, signed |
| 10 | 1 | reason: 0 periodic, 1 degraded, 2 only an alternate answered or protocol 1, 3 timeout, 4 disconnect, 5 switch failed |
| 11 | 1 | current channel |
| 12 | 1 | first alternate channel |

## Radio packets

ESB payloads, first byte the type. Records are `{length, type, data[length]}`.

| Type | From | Content |
| --- | --- | --- |
| `0xE0` | puck | pairing offer: puck ID u32, puck serial (16 bytes, zero padded) |
| `0xE1` | puck | connection request, 18 bytes, no acknowledgement wanted: puck ID u32, controller ID u32, channel u32, the puck's base address (4 bytes), the slot's pipe prefix |
| `0xE2` | puck | hello (also the pairing beacon) |
| `0xE3` | puck | keep-alive, followed by queued records |
| `0xE4` | puck | channel change: the channel list |
| `0xE5` | puck | turn off |
| `0xE6` | puck | the PC is asleep |
| `0xE7` | puck | protocol version: `0`, version |
| `0xF0` | controller | pairing request (in an acknowledgement): controller ID u32, serial (16 bytes) |
| `0xF4` | controller | pairing confirmation: controller ID u32, serial (16 bytes) |
| `0xF1` | controller | records |
| `0xF2` | controller | disconnecting |
| `0xF3` | controller | protocol version |

Records:

| Type | From | Data |
| --- | --- | --- |
| 1 | puck | SET feature report: the report's bytes from the opcode on, `opcode, length, payload`, in all `length + 2` bytes |
| 2 | controller | result of the SET: a signed 32-bit code, so the record length is 4 |
| 3 | puck | GET feature report: the opcode, 1 byte |
| 4 | controller | the answer: up to 63 bytes, the first is the opcode |
| 5 | puck | output report, ID included, up to 64 bytes |
| 6 | controller | input report, ID included; the length must equal the report's size + 1 |

Input records with IDs `0x40`..`0x45` pass to the PC when their length is right; `0x41` only
without modifiers and only with the keys Enter, Escape, Tab and the four arrows; `0x60` and `0x61`
go to the combiner; any other ID is dropped.

## Pairing record

24 bytes, also what vendor commands `0xA3` and `0xA2` read and write: puck ID u32 (random, chosen
when pairing), controller ID u32, controller serial 16 bytes. A zero in either ID means no pairing.

## Vendor command replies

Command replies are `02 <opcode> <length> <data>`. The commands are listed in
[puck-internals.md](puck-internals.md#vendor-command-channel-feature-report-2).

- `0x83`: five `{tag u8, value u32}` pairs: 1 product ID, 2 capabilities (0), 10 bootloader build
  time, 4 firmware build time, 9 board revision. Length 25.
- `0xAE`: the reply starts with the tag requested, then the string, zero padded to 20 bytes in all.
  Tags 0 and 4 board serial, 1 unit serial, 3 revision string, others `NA`.
- `0xF2` selector 0: build time u32, board revision u32, revision string (16 bytes), unit serial
  (16 bytes): 40 bytes. Selector 1: the last crash record, 33 bytes, all zero if there never was one.
- `0xF3` selectors 0 to 3: 40 bytes each, for channels 0, 2, .. 78: the channel number, the packet
  error rate in percent, the background RSSI, its peak. Length 41 with the selector.
- `0xB4`: 1 byte, 2 connected, 1 not.

## Timing

See the timing summary in [docs/04 of the research repository](https://github.com/ded811/steam-puck-reverse-engineered/blob/main/docs/04-radio.md#timing-summary).
