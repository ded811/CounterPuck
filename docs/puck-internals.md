# Inside the puck firmware

What the Steam Controller 2 puck's firmware does, as far as static analysis of the image Steam
ships (`PROTEUS_FW_6A628359.fw`, see [firmware-format.md](firmware-format.md)) can tell. This is
the behavioural and protocol information needed to interoperate with the puck or the controller;
no code from the firmware is reproduced here.

As of October 2026, build `0x6A628359`. Everything here was read from the firmware image, not
measured on hardware; facts marked *inferred* go beyond what the code itself shows.

## Software stack

- **Zephyr RTOS** (3.7.99) via **nRF Connect SDK 2.9.0**, built for the nRF52833. The image uses
  Zephyr's newer USB device stack (`usbd`), its shell and logging over the CDC-ACM port, the
  settings/NVS subsystem for persistent data, SMF state machines, and Nordic's **Enhanced
  ShockBurst (ESB)** library for the 2.4 GHz link. About a fifth of the code is Valve's own.
- **Built from Valve's own SDK forks.** The banner's commits (`nRF Connect SDK v2.9.0-d93dcad627bd`,
  `Zephyr OS v3.7.99-21bb7021858d`) are not in Nordic's public repositories (the official v2.9.0
  release is `7787b2649840` / `1f8f3dc29142`). Differences from stock NCS 2.9.0 that show in the
  image:
  - **USB HID class**: each HID device has two extra callbacks (bus suspended / resumed), and input
    reports are copied into a pool of 8 × 64-byte buffers per interface when they are submitted
    (a full pool returns `-ENOMEM`); stock NCS 2.9.0 queues the caller's buffer without copying it.
    The class also copes with a HID device that registered no callbacks, and an instance whose
    report descriptor was never registered adds no USB interface.
  - **USB stack logging**: 22 per-transfer and descriptor-dump `LOG_INF` messages of stock NCS 2.9.0
    (`Handle control …`, `spool …`, `s-in-status finished`, `Allow data OUT`,
    `Set Configuration Request value …` and others) are not in the image, although those log
    modules run at INF. 13 of them are exactly upstream Zephyr commit
    [`0f8e72a7c2c0`](https://github.com/zephyrproject-rtos/zephyr/commit/0f8e72a7c2c04d9bc79e3c11e765396a293d4110)
    ("usb: device_next: reduce log level to debug for internal status logs", merged in March 2026),
    so Valve most likely backported it. The other nine, and the rest of what the stack logs for a
    rejected control request, Valve silenced itself. Logs and the shell share the CDC-ACM port,
    and on stock NCS 2.9.0 every control request logs several of these lines, so Steam's
    feature-report traffic would keep that port busy (*inferred*). With them gone, the puck's port
    stays quiet.
  - **USB device driver**: every endpoint event the nRF USB driver hands to the stack is checked,
    and a refusal stops the firmware (error log `UDC_SUBMIT_EP_EVENT %d`, then a kernel oops)
    instead of being ignored as in stock NCS 2.9.0.
  - **CDC-ACM**: separate receive and transmit buffer pools (2 × 512 bytes each) instead of one
    shared pool (presumably so that heavy output on the shell/log port cannot starve its input).
  - **Shell**: the shell takes its output lock with a 50 ms timeout instead of waiting forever, so
    shell output from another thread is dropped, rather than blocking for good, when the CDC-ACM
    port is not being read. This is exactly upstream Zephyr commit
    [`b0a0febe5896`](https://github.com/zephyrproject-rtos/zephyr/commit/b0a0febe589662bddb4bfe40c0faaf5795812fb1)
    ("shell: fix unsafe API calls…", January 2025), which is newer than NCS 2.9.0.
  - **Kernel**: each device's state has a second flag bit, and at boot the kernel skips a device
    that has it set, logging the warning `%s is disabled, skipping it`. Nothing in the puck
    firmware sets the bit, so on the puck it never fires.
  - **Sensor API**: Valve's `enum sensor_channel` has two extra values. `SENSOR_CHAN_DIE_TEMP` is
    13 instead of 12 and `SENSOR_CHAN_ALL` is 62 instead of 60, as the on-chip temperature driver
    (used for clock calibration) shows. Only the values are known, not the new channels' names.
  - **C library**: full newlib, not newlib-nano (NCS 2.9 defaults to picolibc). Its strings and
    its full printf core are in the image.

  Each source change above was established by compiling the stock NCS 2.9.0 source, with and
  without the candidate change, using the same compiler, and comparing the machine code with the
  image function by function (ignoring only the addresses the linker fills in): with the change,
  every function involved matches byte for byte. Two of the changes are later upstream Zephyr
  commits that Valve evidently backported (13 of the log demotions, and the shell timeout). The
  rest are Valve's own: they are in neither upstream Zephyr nor Nordic's sdk-zephyr, including
  the NCS 2.9.1–2.9.3 patch releases. The C library is configuration, not a source change:
  newlib's functions were located in the image with a coarser comparison (93–100% similar). The
  13 upstream log demotions were found by their missing strings; with all the demotions applied,
  the functions that contain them match too.
- **Build configuration** that shows in the machine code:
  - Thread names are compiled out: `k_thread_name_set` just returns `-ENOSYS`, so the names the
    code gives its threads (the radio slots' `esb/ibex_<n>`, for example) never reach the kernel.
  - Thread runtime statistics, with usage analysis, are on.
  - Every peripheral interrupt is connected at priority 3, not Zephyr's nRF default of 1. GPIOTE
    keeps its usual 5, and the ESB library sets its own (RADIO 1, EGU0 and TIMER2 2).
  - The FPU stays off; floating-point maths is done in software. MPU null-pointer detection is on.
  - Logging is deferred, with an 8 KB buffer, and the log thread runs 1 ms after a message is
    posted (Zephyr's default is 1 s). There are two backends: the shell on the CDC-ACM port and
    SEGGER RTT (one 4 KB up buffer, "Terminal"). The CDC-ACM class's own log module is compiled
    out.
- Valve's own modules, by their log-module names: `main`, `usb`/`neptune_usb` (USB bring-up;
  "Neptune" is the Steam Deck's codename, so *inferred* to be code shared with it), `hid_custom`
  (vendor HID commands), `dongle_esb`/`esb_common`/`radio`/`cost` (radio slots, link, channel
  selection), `hid_proxy`/`hid_client` (the USB HID interfaces), `puck`/`gpio_puck` (the dock),
  `rr` (reset reason), `frest` (factory restore), `watchdog`, and `board_hid_over_i2c`/
  `ec_input_tap`/`i2c_hid` for the **Nereid** variant. Valve's settings-recovery code logs under
  Zephyr's own `settings` module, and `hid_api` is Zephyr's USB HID class module.
- The image has a 40-byte header at offset `0x100` (right after the vector table): two magic
  words, a 12-character git short hash, the build timestamp, and three words of unknown meaning,
  one possibly a version (see [firmware-format.md](firmware-format.md#app-header-payload-0x100)).
  The IBEX (controller) image has the same header at the same offset, identical except for its
  own build time, so both were most likely (*inferred*) built from one commit of one repository.

## Boot and the bootloader handshake

The application is linked at `0x8000`; the bootloader owns the first 32 KB and is never changed
by updates. The two talk through a mailbox at `+0xF0`–`+0xFF` of the top 256 bytes of RAM
(`0x2001FF00`–`0x2001FFFF`, a no-init region that survives a warm reset):

| Direction | Where | Content |
| --- | --- | --- |
| bootloader → app | RAM `0x2001FF00 + 0xF0` | magic `0xCD595B80`, and at `+0xF8` the bootloader's build time (reported to the host as attribute 10) |
| app → bootloader | same block | `+0xF0` = `0xCD595B80`, `+0xF4` = `0x93541DA0`, `+0xF8` = `0x0078143C`, `+0xFC` = 0, then a warm reboot 20 ms later (656 ticks of the 32768 Hz kernel clock): "stay in DFU" |

That is what HID command `0x90` does, and why the puck re-enumerates as the `28DE:1007` CDC
bootloader during a Steam update. The reset-reason record lives at the start of the same region
(`+0x00`–`+0x24`, magic `0x82FE5342`): it is written on a fatal error just before the reset, and
at the next boot the app copies it to settings (`debug/resetreason`) and then clears it.

## USB device

| Field | Value |
| --- | --- |
| VID:PID | `28DE:1304` ("Steam Controller Puck", "Valve Software"); `28DE:1305` on Nereid hardware |
| Class | `EF/02/01` (composite with interface association descriptors), bcdUSB 2.01 with a BOS USB 2.0 extension, bcdDevice 2 |
| Interfaces | CDC-ACM (shell/log port), then **HID_0..HID_3** (one per radio slot), then **hid-puck** (the dock) |
| Serial string | the unit serial from UICR, placeholder `XXXXXXXXXXXXX` when unprovisioned |

Each HID_n report descriptor is three collections: a **mouse** (report `0x40`, 5 bytes: buttons,
X, Y, wheel, AC pan), a **keyboard** (report `0x41`, 8 bytes) and a **vendor** collection (usage
page `0xFF00`) with input reports `0x42` (53 B), `0x44` (5), `0x79` (1), `0x43` (14), `0x7B` (12),
`0x45` (45), output reports `0x80`–`0x89`, and two 63-byte **feature reports, IDs 1 and 2**. The
hid-puck descriptor is the vendor collection only, with inputs `0x42`, `0x79` and features 1, 2.

- **Feature report 1** is relayed to the controller (the original Steam Controller's
  feature-report protocol): over the radio on HID_n, over the dock UART on hid-puck.
  `SET_REPORT` must be 64 bytes (anything else is rejected); over the radio only
  `<opcode> <len>` and the `len` payload bytes are sent. If no controller is connected (HID_n) or docked
  (hid-puck), the request stalls. `GET_REPORT` is non-blocking: it stalls until data is ready,
  and the host retries it. On HID_n it needs a preceding SET: the first GET sends the request,
  later GETs stall until the controller's reply to the SET's opcode arrives and then return 63
  bytes; after 1 s without a reply the request is dropped and GETs keep stalling until the next
  SET (a reply for a different opcode also drops it). On hid-puck any GET with nothing outstanding sends a UART GET (no SET needed), and a later
  GET returns the 63-byte reply; there is no timeout, but undocking makes it fail.
- **Feature report 2** is the puck's own command channel, below.
- **Input report `0x79`** (1 byte) is the link state of that interface: 1 disconnected,
  2 connected, 3 just paired (on hid-puck: 2 docked, 1 undocked).
- **Input report `0x7B`** (12 bytes, little-endian) carries link statistics while connected:
  `[0..1]` u16 length of the statistics window in ms (saturates at `0xFFFF`), `[2..3]` u16
  successful exchanges, `[4..5]` u16 failed transmit attempts on the primary channel (this can
  exceed the exchange count), `[6..7]` u16 longest gap between successful exchanges in ms, `[8]`
  i8 average RSSI in dBm (0 if nothing succeeded, floor −128), `[9]` reason, `[10]` and `[11]`
  the first two entries of the slot's channel list (current channel, first alternate or `0xFF`).
  Reasons: 0 periodic (sent with the first successful exchange after the interval, 500 ms by
  default, which the `connection_qos` shell command can change), 1 channel switch because of
  the link history, 2 channel switch after a success on an alternate channel (or a protocol-1
  `0xF3`), 3 connection timeout, 4 the controller is disconnecting (`0xF2`), 5 channel switch
  failed. Reasons 1–5 are sent immediately, and a 1 or 2 can be followed at once by a 4 or 5.
  Every report resets the counters and starts a new window.
- The dongle only forwards keyboard reports containing arrow keys, Enter, Escape or Tab with
  no modifiers; anything else is dropped ("lizard mode" is deliberately minimal).

## Vendor command channel (feature report 2)

`SET_REPORT` with `02 <opcode> <len> <payload…>` (64 bytes); the following `GET_REPORT` on the
same interface returns `02 <opcode> <len> <response…>`. Twelve opcodes are implemented. Five
are the original Steam Controller's and have names in SDL's `controller_constants.h` (`0x83`,
`0xAE`, `0xB4`, `0x86` factory reset, `0xAD` enable pairing); the other seven are not in that
header.

| Opcode | Direction | Meaning | Details |
| --- | --- | --- | --- |
| `0x83` | get | attribute values | 5 × `{tag u8, value u32}`: 1 product ID `0x1304`; 2 capabilities 0; 10 bootloader build time; 4 firmware build time; 9 board revision (read from UICR, `0x47` on this unit) |
| `0xAE` | get | string attribute | payload tag: 1 unit serial, 0 or 4 PCBA serial, 3 git hash, otherwise `NA`; 20 bytes |
| `0xB4` | get | wireless state | 1 byte: 1 disconnected, 2 connected (on hid-puck: 2 docked, 1 undocked) |
| `0xA3` | get | read pairing record | 24 bytes: `{proteus_uuid u32, ibex_uuid u32, controller serial[16]}` |
| `0xA2` | set | write pairing record | same 24 bytes; if either UUID is zero the record is deleted instead; any write restarts the slot (CONNECTING if a record remains, otherwise idle), except a slot in pairing mode, which ignores the restart |
| `0xAD` | set | pairing mode | `[0]` non-zero opens, 0 closes; `[1]` = seconds (u8, 0 means 60) |
| `0x90` | set | reboot into the bootloader | see the handshake above |
| `0x95` | set | reboot | payload `0xECAABAC0` → bootloader, anything else → normal reboot |
| `0x86` | set | factory reset | only with payload `0xEFC1A917`; deletes the settings subtrees `user`, `esb` (pairing), `mte`, `debug` and `bt` (`cal` is kept), then reboots about 110 ms later |
| `0xF1` | set | dock input streaming | meant for hid-puck: `[0]` non-zero enables input report `0x42` from the docked controller. The firmware does not reject it on HID_0..3, where the handler uses a null device configuration: do not send it there |
| `0xF2` | get | device info | `[0]` = 0: build time, board revision, git hash, serial (40 bytes, 41 with the selector); `[0]` = 1: the 33-byte reset-reason record (34 bytes with the selector), after which the record's "new" flag is cleared; other values return nothing |
| `0xF3` | get | per-channel statistics | `[0]` = 0 channel numbers, 1 PER %, 2 RSSI, 3 peak RSSI; 40 values for channels 0, 2, … 78 |

`0xA3`, `0xA2` and `0xAD` act on the radio slot behind the HID_n interface they are sent to. On
hid-puck they do nothing (`0xA3` returns an empty response).

## The radio link

- **Modulation**: ESB with dynamic payload length, puck as the transmitter (PTX), bit rate
  enum "2 Mbps BLE" (ESB on the 2 Mbit BLE-compatible PHY), 16-bit CRC, +6 dBm, no automatic
  retransmits (the retransmit count is set to 0, so the 600 µs retransmit delay is unused; the
  slot code retries in software), payloads up to 128 bytes, selective auto-ack, fast ramp-up.
- **Addressing**: ESB base address 0 is the four bytes `"ibex"` (shared by every puck and
  controller: the "common" address). Base address 1 is four random bytes chosen at every boot
  (bytes `0x00`, `0xAA`, `0x55` re-rolled). Five pipes: pipe 0 on the common address, pipes 1–4
  on the puck's own address, one per slot, each with a one-byte prefix. The prefixes start as
  `0x10` for pipe 0 and `0x01`–`0x04` for pipes 1–4. A slot's prefix is incremented by `0x10`
  every time the slot enters CONNECTING, so stale controllers fall off (its first connection
  after boot already uses `0x11`–`0x14`). Pipe 0 keeps `0x10`, also while base address 0 is
  swapped for pairing.
- **Channels**: even channels only, 2–80 (2402–2480 MHz) in all. Channel selection works over
  4–80 (2404–2480 MHz): each slot keeps a per-channel packet error rate, and one global
  background thread samples the noise level (RSSI) on every one of these channels every 128 ms,
  for all slots (it takes turns with them on the radio). A cost function
  with four tunable weights picks the lowest-cost channel, ties going to the higher channel:
  distance from the current channel, PER, RSSI IIR and RSSI peak, by default 1000, 1000, 500
  and 0 (the peak term is off unless set with the `channel_cost` shell command). The data
  channel is always one of 4–78: channel 80 is never picked, and when switching, the current
  channel is excluded and channels up to 16 MHz from it are penalised, more the closer they
  are. Channel 2 carries pairing and connection requests, and 2 and 80 are the standing
  alternates.
- **Indicator outputs**: each slot has one active-low output that is held low for the whole of
  each of its radio transactions (slots 0–3: P0.20, P0.28, P0.29, P0.15), and P0.10 is driven
  high while each channel is sampled during the background RSSI scan (one short pulse per
  channel). What they connect to on the board is unknown
  (activity LEDs or test points, *inferred*); the radio protocol does not depend on them.
- **Packet**: a message type byte followed by payload. Puck → controller types: `0xE0` pairing
  offer, `0xE1` connection request, `0xE2` hello / pairing beacon, `0xE3` keepalive, `0xE4` channel
  change (the new channel, followed by the alternates 2 and 80 for a protocol-1 controller), `0xE5` turn
  off, `0xE6` host asleep, `0xE7` protocol version (currently `0, 1`). Controller → puck (in ack
  payloads): `0xF0` pairing request, `0xF4` pairing acknowledgement, and in connected mode
  `0xF1` application data, `0xF2` disconnecting, `0xF3` protocol version.
- **Application data** is a list of `{length, type, data}` records. The puck appends its
  records to `0xE3` keepalives, as many queued host messages as fit in 128 bytes: type 1 SET
  feature report, 3 GET feature report, 5 output report. The controller answers in the ack
  payload with `0xF1` followed by records: type 2 the SET's 4-byte status reply, 4 the GET
  response (≤ 63 bytes), 6 an input report (report ID first). Type-6 records reach USB only for
  report IDs `0x40`–`0x46` whose length exactly matches the report descriptor (`0x41` must also
  pass the keyboard filter above); `0x60`/`0x61` go to the combiner below, and any other ID is
  dropped.

### Pairing (HID `0xAD`, 60 s by default)

1. The puck sends a `0xE2` beacon every ~10 ms on **channel 2**, pipe 0, with base address 0
   temporarily set to `91 A2 A7 93`. A controller in pairing mode answers in the ack payload with
   `0xF0 {ibex_uuid u32, serial[16]}`.
2. The puck draws a random `proteus_uuid` and sends `0xE0 {proteus_uuid, puck serial}` on
   channel 2, pipe 0, now addressed with the controller's `ibex_uuid` as base address 0. The
   controller acknowledges with `0xF4 {ibex_uuid, serial}`.
3. The record `{proteus_uuid, ibex_uuid, serial}` is saved under settings key `esb/ibex_<n>`
   (n = slot 0–3) and the slot goes to CONNECTING.

Entering pairing first erases that slot's existing record. A timeout, closing pairing mode
(`0xAD` with `[0]` = 0) or a host suspend ends pairing and leaves the slot unbonded and idle.

### Connecting and connected

- Every 4 ms (up to 250 times, then every 100 ms): a no-ack `0xE1 {proteus_uuid, ibex_uuid,
  channel u32, puck base address 1, slot pipe prefix}` on channel 2 / common address, a 1 ms
  pause, then a `0xE2` hello on the chosen channel and the slot's pipe. An acknowledged hello
  means connected; the puck sends `0xE7 {0, 1}` once, then `0xE3` keepalives.
- A keepalive that gets no acknowledgement is retried on the current channel for 10 ms, then in
  turn on the alternate channels and the current one; if nothing answers within 500 ms the link
  is considered lost ("Connection timeout") and the slot goes back to CONNECTING. From each
  connection until its first channel switch there are no alternates, so only the current
  channel is tried. Three misses within the last eleven exchanges, or success only on an
  alternate channel, trigger a channel switch: the puck picks the lowest-cost channel and sends
  a protocol-1 controller `0xE4 {new, 2, 80}`, trying the old and new channels for 10 ms and
  then all four for 500 ms; on success channels 2 and 80 become the alternates for the next
  outage. A protocol-0 controller gets just `0xE4 {new}`, retried over the old and new channels
  for 500 ms, and is left with no alternates.
- When a controller reports protocol 1 (`0xF3`), the puck switches channel immediately, which
  installs the alternates 2 and 80.
- A second thread ("roybex_combiner") merges two compact radio-side reports (`0x60`, `0x61`,
  19 bytes each) from the controller into the 45-byte HID input report `0x45`.
- **USB suspend**: when the host suspends, the slot starts a 2 s timer; on expiry it sends
  `0xE5` so the controller powers down. If the controller is still answering 2 s after that, the
  puck drops the link ("Ibex took too long to shutdown") and, on that slot's next connection
  while the host sleeps, neither wakes the host nor sends `0xE6`. Otherwise, when a controller
  connects while the host is asleep the puck issues a USB remote wakeup and waits up to 10 s for
  the host; if the host doesn't wake in that time the puck sends `0xE6` (host asleep) and keeps
  the link.

## The dock

The three pogo pads are `+`, `-` and `P`. `P` is **"pilot"**: *inferred* to be one net that
carries both an analog presence level from the controller and a half-duplex UART.

- VPILOT is measured on SAADC AIN1 (P0.03) and scaled ×2.433 (a resistor divider with
  R_top:R_bottom ≈ 1.433:1).
  While idle the dock samples it every 100 ms; once a reading is inside **1670–3333 mV** it
  samples every millisecond, and 200 consecutive in-window readings mean a controller is seated.
- Then **VPOGO_EN (P0.11)** switches the `+` rail on (about 5 V, *inferred* to be USB VBUS from
  its 4.2–6.0 V window; sensed as VPOGO on AIN0 (P0.02) with the same scaling), and a monitor
  re-checks both rails every 5 ms. VPOGO outside **4.2–6.0 V** cuts the rail and leads to a
  fault (10 s cool-down). VPILOT outside its window cuts the rail and goes straight back to
  idle, but that check is skipped while a UART transfer is in progress.
- About 1 s after switching the rail on (a rail fault in that second ends detection instead),
  the puck sends a probe frame over the UART (TX P0.04, RX P0.05) and discards the first frame it
  receives (presumably its own echo) before reading the reply. No reply → power off and a 10 s cool-down.
  *Inferred*: the VPILOT input P0.03 and both UART pins reach the `P` pad through passives (the
  echo and the skipped VPILOT check point that way); confirm with a meter.
- UART frames are 73 bytes: `0x5A`, a request type, 69 bytes of payload and a little-endian
  CRC16-CCITT over the first 71 bytes. Request types sent: 0 probe (all-zero payload), 1 GET
  feature report, 2 SET feature report, 3 GET input report. Each transfer waits up to 100 ms for
  its own echo, then 64 ms for the reply, then pauses 10 ms; a failed transfer is retried once.
- While attached, feature report 1 on hid-puck is relayed over this UART (see above) and report
  `0x79` tells the host 2 (attached) / 1 (detached). Whenever 64 ms pass without a host request
  the puck polls type 3; the reply goes to the host as input report `0x42` only if streaming is
  on. HID command `0xF1` only sets or clears that flag (nothing is sent to the controller);
  turning it off also sends one all-zero `0x42` report.

## Identity and persistence

- The unit serial and the PCBA serial are stored in the nRF52833's **UICR CUSTOMER** registers:
  `CUSTOMER[0]` is the magic word `0xAC32A429`, `[1]` the board revision, `[2..5]` the unit
  serial (`FXB…`) and `[6..9]` the PCBA serial (`MXA…`). Each serial must be 13 characters
  `0-9`/`A-Z` followed by a NUL, otherwise it reads as `XXXXXXXXXXXXX`; without the magic the
  board revision reads as `0xFFFFFFFF`.
- The **board revision** (`0x47` on this unit) is not a constant, and it changes behaviour: the
  dock hardware (the pogo UART and its driver) is brought up only on revisions `0x46` and `0x47`;
  `0x44` is Nereid (below), and it is the one revision on which the firmware leaves the
  regulator setting alone (see [Power and clocks](#power-and-clocks)).
- Settings: at boot the firmware loads the subtrees `cal`, `user`, `esb` (pairing records) and
  `mte`; the reset-reason log is stored as `debug/resetreason`; factory reset deletes `user`,
  `esb`, `mte`, `debug` and `bt`. Settings handlers exist only for `esb` (registered at run
  time) and `debug/resetreason`, so `cal`, `user` and `mte` have no handler in this build and
  `bt` is only ever deleted. The settings partition is the last 12 KB of flash (`0x7D000`); if settings
  initialisation fails with `-EDEADLK` (NVS finds every sector closed, i.e. the partition is
  corrupt or not NVS) the firmware erases the whole partition and initialises it again.
- A watchdog with a 5 s window is fed every 2.5 s from a background work queue (`bg_work`). If
  feeding stalls for 4.5 s, an item on the system work queue logs "Watchdog about to expire" and
  raises a fatal error, which goes into the reset-reason record before the reset. The watchdog
  resets the whole chip, and it pauses while a debugger has the CPU halted.

## Hardware for compatible boards

What the firmware says about the board it runs on, for anyone designing hardware that should run
puck-compatible firmware. All of it was read out of the application image. Nothing was measured
on a board, so check every net with a meter before relying on it. Board markings are from
[iFixit's teardown](https://www.ifixit.com/Teardown/Steam+Controller+(2nd+Generation)+Wireless+Puck+Teardown/217571).

### Pins

| Signal | nRF52833 pin | What the firmware does with it |
| --- | --- | --- |
| VPOGO_EN | P0.11 | Output, active high, starts low: switches the `+` pogo rail |
| Pogo UART TX | P0.04 | UARTE0, 115200 baud 8N1, no flow control; reaches the `P` pad (*inferred*) |
| Pogo UART RX | P0.05 | Internal pull-up; reaches the `P` pad (*inferred*) |
| VPILOT sense | P0.03 (AIN1) | `P` pad through a divider; window 1670–3333 mV at the pad (0.686–1.370 V at the pin) |
| VPOGO sense | P0.02 (AIN0) | Switched `+` rail through a divider; window 4200–6000 mV (1.726–2.466 V at the pin) |
| Slot 0–3 activity | P0.20, P0.28, P0.29, P0.15 | Outputs, active low, held low during each radio transaction of that slot |
| RSSI scan | P0.10 (NFC2) | Output, high while the background scan samples a channel |
| Reset | P0.18 | Pin reset |
| NFC1 | P0.09 | Plain GPIO, unused |
| Nereid I2C | P0.11 SDA, P1.09 SCL, P0.17 interrupt | Board revision `0x44` only, see [Nereid](#nereid) |

Both analog inputs are sampled at gain 1/6 against the internal 0.6 V reference, 12 bits, no
oversampling, and the firmware multiplies each reading by 2.433. So both dividers are
R_top : R_bottom = 1.433 : 1 (14.3 kΩ over 10 kΩ would fit; the real values are unknown). What
the five indicator outputs drive on the board (LEDs, test points) is also unknown; the radio
protocol does not depend on them. On every board revision the startup code switches the NFC
pins to GPIO and enables pin reset on P0.18 in UICR, each followed by a reset, if they are not
set that way yet.

### Power and clocks

- **Supplied on VDDH, VDD = 3.3 V** (*inferred*). At boot, if the chip reports that it runs from
  VDDH and `UICR.REGOUT0` is still at its erased default (1.8 V), the firmware programs REGOUT0
  for 3.3 V and resets, once. It does this on every board revision except `0x44`. So the
  nRF52833 is most likely powered from USB VBUS on VDDH, and VDD is a 3.3 V output for the rest
  of the board. Measure VDD before connecting a debug probe, and use a probe that follows VTref
  rather than one fixed at 3.3 V: a blank chip in this mode runs VDD at 1.8 V until that first
  boot.
- **No DC/DC.** The firmware never enables REG1's DC/DC converter, and the nRF52833 has none on
  its VDDH stage, so this firmware needs no DC/DC inductor.
- **No 32.768 kHz crystal.** The low-frequency clock comes from the internal RC oscillator, with
  Zephyr's calibration checking the on-chip temperature every 4 s; P0.00 and P0.01 are unused.
  The only crystal is the 32 MHz one (X1) that the radio and USB need.

### Memory

The flash map is in [firmware-format.md](firmware-format.md#load-address-and-flash-map-puck):
bootloader in the first 32 KB, the application from `0x8000`, settings in the last 12 KB. In RAM
the application uses the first 127 KB (`0x20000000`–`0x2001FBFF`), `0x2001FC00`–`0x2001FEFF` is
unused, and the top 256 bytes (`0x2001FF00`–`0x2001FFFF`) are the no-init page shared with the
bootloader (the mailbox and the reset-reason record, see
[above](#boot-and-the-bootloader-handshake)).

### Debug port

The chip's marking, `QDAAB0`, makes it a revision 2 nRF52833 (build code B0). On revision 2 the
debug access port is locked after every reset unless both UICR and the running firmware open it
(Nordic's information notice IN-149). The application's startup code, Nordic's standard
`SystemInit`, copies `UICR.APPROTECT` into the `APPROTECT.DISABLE` register and never writes
`FORCEPROTECT`. So the application neither locks the port nor opens it on purpose: it is open
after boot only if the factory left `UICR.APPROTECT` at `0x5A` (HwDisabled). The bootloader runs
first and is not in any Steam file, so what it does here is unknown. Only connecting a probe,
read-only, can tell. I haven't done that yet.

If you try it on your own puck: a locked nRF52 can only be unlocked by erasing the whole chip,
which wipes the bootloader (not in any Steam file), the application, the pairing records and the
serial numbers in UICR. Some tools do that without asking. pyOCD mass-erases a locked nRF52 when
it connects unless its `auto_unlock` option is set to false, and `nrfjprog --recover` erases it
by design. Connect read-only, and if the port reports that it is protected, stop there.

## Nereid

Board revision `0x44` turns the same firmware into **Nereid**: the USB PID becomes `0x1305`, and
besides being a USB device the firmware also acts as an **I2C target at address `0x50`**,
speaking HID over I2C to an embedded controller (EC). *Inferred*: the puck's chip and firmware
built into another product; which product is unknown.

- **Bus**: the nRF52833's TWIS (I2C target) peripheral on SDA P0.11 and SCL P1.09, with no
  internal pull-ups, plus an interrupt line to the EC on P0.17 (active low, open drain). The
  TWIS driver is Valve's own: the Zephyr in NCS 2.9 has none (upstream only added one in Zephyr
  4.1, and Valve's shares none of its strings).
- **HID over I2C**: HID descriptor at register 1 (version 1.00, VID `0x28DE`, PID `0x1305`),
  report descriptor at 2, input at 3, output at 4 (no output reports), command at 5 (only RESET
  is implemented) and data at 6 (refused). Input reports are 8 bytes (a 2-byte length and up to
  6 bytes of report). Up to 8 are queued, and the interrupt line stays asserted while any are
  waiting.
- **What the EC gets**: four "EC input tap" devices, one per radio slot, mirror the slot's USB
  input reports, which still go to USB as well. Report `0x42` or `0x45` becomes report `0x78`
  `{slot, 4 bytes}` with bytes 2–5 of the original (byte 0 being the report ID), sent only when
  those bytes change. Report `0x79` (link state) becomes `0x7A` `{slot, state}`.
- **Revision `0x44` only.** On the retail puck (`0x47`) none of it is initialised: the TWIS, its
  pins, the HID-over-I2C device and the taps stay off, which leaves P0.11 free to be VPOGO_EN.
  The dock, in turn, only runs on `0x46` and `0x47`.

## Shell

The CDC-ACM port is a Zephyr shell with the prompt `dongle~$ `, built minimal: Tab completion,
but no history and none of Zephyr's built-in commands (no `log`, `kernel` or `device`). Besides
`help`, the commands are `bonds`, `connection`, `connection_qos`, `connection_qos_stop`,
`packet_error_rate`, `channel_cost`, `channel_cost_show`, `background_rssi_show`,
`radio_send`, `radio_send_channels`, `factory_restore --really`, `resetreason_print`,
`resetreason_clear`, and three `generate_exception_*` test commands.
Log output (INF and up) goes to the same port and to SEGGER RTT; the USB stack itself stays quiet
(see [Software stack](#software-stack)).
