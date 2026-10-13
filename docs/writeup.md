# How Steam updates the Steam Controller 2 puck

*October 2026. Reference material lives in [firmware-format.md](firmware-format.md) and
[update-protocol.md](update-protocol.md); [reproduce.md](reproduce.md) shows how to repeat
everything here.*

I wanted to understand the Steam Controller 2's wireless puck well enough to eventually build
my own. I expected the hard part to be getting the firmware off the chip. It turned out Steam
had already put it on my PC, unencrypted. Watching one real update then filled in the rest.

Everything below came from files on the PC and passive USB captures, on Windows, with one
puck and one update. I didn't open or probe the hardware.

## 1. What the puck looks like from the PC

Plugged in, the puck enumerates as `28DE:1304`, "Steam Controller Puck" by Valve Software. It
is a composite device:

- one CDC serial interface (it shows up as a COM port), and
- five HID interfaces. Four of them (`MI_02`–`MI_05`) each carry a mouse, a keyboard and a
  vendor-defined collection. The fifth (`MI_06`) carries only a vendor collection.

Steam's own log (`Steam\logs\controller.txt`) shows it opening the vendor collection on all
five, notes "Controller uses V1 HID protocol via Dongle", and logs `Docked slot = true` for the
fifth. Four radio slots plus one docked slot fits a puck that pairs up to four controllers.
The log also names a work item `CTritonDockedToPuckWorkItem`, which is the first sign of
Valve's codename for the controller: **Triton**.

The chip is a Nordic **nRF52833** (marking `N52833 QDAAB0`), on a board silkscreened
`ZV7_E5K5_Puck_Board R01`. These details come from the photos in
[iFixit's teardown](https://www.ifixit.com/Teardown/Steam+Controller+(2nd+Generation)+Wireless+Puck+Teardown/217571) of the puck (steps 5 and 7), not from mine.

## 2. The firmware was already on disk

Searching Steam's install folder for anything firmware-shaped found
`Steam\bin\hardwareupdater\`:

```text
hardwareupdater.exe
hardwareupdater.cfg
IBEX_FW_6A628345.fw
PROTEUS_FW_6A628359.fw
```

The hex suffixes are Unix timestamps: both images were built on 2026-07-23. The config file
uses the same timestamps to name the current build of each product, plus a "must update"
floor of 2026-05-28.

Strings told the two images apart. `PROTEUS_FW` boots with `Starting Proteus BUILD_TIME_…`,
contains `hid-puck`, Nordic's USB device driver, four radio slots (`esb/ibex_0`..`3`) and
the `28DE:1304` USB descriptor. So **Proteus** is the puck. `IBEX_FW` boots with
`Starting Triton…` and contains `Connecting to: Proteus %s`. So **Ibex** is the
controller's radio firmware.

## 3. Reading the container

Both files start with the same shape of 32-byte header:

| Word | PROTEUS | What it turned out to be |
| --- | --- | --- |
| 0 | `0x2E795631` | A per-product magic (explained by the updater, below) |
| 1 | `0x00030558` | The payload length: exactly the file size minus 32 |
| 2 | `0x747A4771` | A standard CRC-32 of the payload |

After the header is a normal Cortex-M vector table: a stack pointer in RAM (`0x20015D00`) and
Thumb-mode handler addresses. The data averages 5.4 bits/byte of entropy, and readable strings
are everywhere. That's ordinary compiled code, neither encrypted nor compressed. The strings
also give away the toolchain: **nRF Connect SDK v2.9.0** and **Zephyr**, from Valve's own forks of
both (the commits they name aren't in Nordic's public repositories).

Details: [firmware-format.md](firmware-format.md).

## 4. Unpacking the updater

`hardwareupdater.exe` ends with PyInstaller's archive marker. I wrote a small static
extractor ([`pyi_extract.py`](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/tools/pyi_extract.py), now in the research repository) that lists and unpacks the
bundle without running anything. Inside is Python 3.14, `hidapi`, `pyserial`, and one
application script, `hardwareupdater.py`.

The script's compiled code can't be loaded by an older Python, but its names and string
constants are readable. Together they describe the whole update:

- Header word 0 is checked against `PROTEUS_FIRMWARE_HEADER_MAGIC` and
  `TRITON_FIRMWARE_HEADER_MAGIC`. Both values appear as constants in the code (`0x2E795631`
  and `0xD2D86467`), so word 0 is a product magic.
- The running device is queried over **HID feature reports** for attributes such as
  `build_timestamp` and `hw_id`, then told to `reboot_to_BL`.
- Bootloaders are found as **serial ports** with `VID:PID=28DE:1005` or `28DE:1007`.
- Messages to the bootloader are framed with `SOF_BYTE`, `EOF_BYTE` and `ESCAPE_BYTE`, and
  are named `MESSAGE_INFO`, `MESSAGE_FW_BEGIN`, `MESSAGE_FW_DATA`, `MESSAGE_FW_END` and
  `MESSAGE_RESET`.
- The device types it knows include a third, unexplained one: **Nereid**, grouped with the
  puck as a "dongle".

That gave the outline. The byte values (which bytes, which IDs, what's in each message) only
came from watching it run.

## 5. Watching a real update

When Steam offered a puck update, I captured it with USBPcap. The capture was limited to the
puck, its hub, and any device that appeared mid-capture, so it also picked up the bootloader
without recording my keyboard. A second, non-admin script logged process launches and USB
device changes alongside.

The process log shows what Steam ran, in order:

```text
hardwareupdater.exe --show-all-devices
hardwareupdater.exe --update-by-serial <puck serial>
hardwareupdater.exe --check-for-updates      (several times, afterwards)
```

On the USB side:

1. **HID, app running.** The updater reads the puck's serial with command `0xAE`, then its
   attributes with `0x83`. The attributes include the app build time (`0x6A4423DE`,
   2026-06-30) and the bootloader build time (`0x68D2F9F2`, 2025-09-23). It then sends
   `0x90`. Within 30 ms the puck drops off the bus.
2. **Bootloader.** A third of a second later a new device appears, `28DE:1007`, with the same
   serial number, as a plain USB serial port. The updater waits about 4 s before opening it,
   then talks to it in frames that start with
   `0xAD` and end with `0xAE`, with `0xAC` as an escape byte. INFO returns a 164-byte block
   with the bootloader's build time, the header of the currently installed app, a
   provisioning magic, a hardware ID and two serial numbers. BEGIN gets an ACK only after a
   10-second erase. Seven DATA messages carry the payload (six of 32 KB and one of 1.3 KB).
   END carries the 32-byte `.fw` header, and RESET finishes it.
3. **App again.** The puck returns as `28DE:1304`. Its attributes now report build
   `0x6A628359`, the one in Steam's folder. The bootloader's build time is unchanged.

The whole update took about 20 seconds.

The decisive check: un-escaping the DATA messages and joining them gives a 197,976-byte image
that is **byte-for-byte identical** to the `.fw` payload, and the END message equals the file's
header. What Steam sends is exactly the file on disk.

I then compared the HID side with SDL, which carries an open-source driver for the original
Steam Controller. Its `controller_constants.h` has the same command IDs (`0x83`, `0xAE`,
`0x87`, `0xB4`). Its attribute enum numbers line up exactly with the tags the puck returned:
product ID, capabilities, firmware build time, board revision and bootloader build time. The
puck reuses the original controller's HID command set. The one command the updater sent that SDL
doesn't name is `0x90`, reboot to bootloader; before the updater ran, Steam also read each slot's
pairing record with `0xA3`, another command SDL doesn't have.

Byte-level details: [update-protocol.md](update-protocol.md).

## 6. Where the code sits in flash

A raw image doesn't say where it's loaded, but its code points at its own strings with absolute
addresses. For every candidate base address I counted how many 32-bit words in the image would
then point exactly at the start of a string
([`tools/find_base.py`](../tools/find_base.py)). `0x8000` scored 491 hits and the next best 44.

So the app is linked at `0x8000`. The first 32 KB of the chip's 512 KB flash belongs to the
bootloader, which matches what the update showed: the bootloader is a separate program that
updates never touch.

## 7. Inside the firmware

With the image loaded in Ghidra at `0x8000`, the strings do most of the work: Zephyr names its
log modules, devices, shell commands and settings keys, and Valve's own code logs generously.
From those, plus the static tables Zephyr emits, it was possible to map out the USB device,
the twelve vendor HID commands (including pairing and bond transfer), the radio configuration
and the pairing/connection sequence, the bootloader handshake through a RAM mailbox, and how
the dock detects and talks to a seated controller. All of that is written up as protocol facts
in [puck-internals.md](puck-internals.md); no firmware code is reproduced.

The SDK commits in the boot banner aren't public, so I also worked out what Valve changed in its
fork. Strings missing from the image, such as per-transfer USB log lines that stock NCS 2.9.0
prints, pointed at candidates. For each one I compiled the stock NCS 2.9.0 source with the same
compiler, with and without the candidate change, and compared the machine code with the image
function by function. A right guess matches byte for byte. That turned up two backported upstream
Zephyr fixes (13 quieter USB log lines, a shell lock timeout) and Valve's own changes: more
quieted USB log lines, the USB HID class, the USB driver, CDC-ACM, the kernel's device start-up
and the sensor API. The list, with the build configuration that also shows in the code, is in
[puck-internals.md](puck-internals.md#software-stack).

The same analysis gives the pin map, power setup and memory split that a compatible board needs,
and it shows that the app leaves the debug port the way the factory configured it
([puck-internals.md](puck-internals.md#hardware-for-compatible-boards)).

## What's still open

- **Bootloader checks.** The file and the messages carry a magic, a length and a CRC32, and no
  signature. Whether the bootloader checks anything more isn't known. I haven't tested it, and
  I won't with the only puck I have.
- **The `0x12` byte** that follows every message ID from the host.
- **`28DE:1005`.** It's presumably the controller's bootloader, but unconfirmed.
- **The debug port.** The app leaves it as UICR says
  ([puck-internals.md](puck-internals.md#debug-port)). Whether the factory left it open needs a
  read-only probe, which I haven't connected.
- **Nereid**, the third device type in the updater: identified in the firmware as board
  revision `0x44` (see [puck-internals.md](puck-internals.md)); the product itself is unknown.
- **The radio link** between puck and controller (Nordic ESB) and pairing are mapped from the
  firmware ([puck-internals.md](puck-internals.md)); confirming them on real hardware is next,
  and it's where [OpenPuck](https://github.com/safijari/openpuck) has already done a lot.
- **Linux and macOS**: I only looked at the Windows client.

The app build the puck shipped with (2026-06-30) is gone: the update overwrote it, and it was
never in Steam's folder while I watched. Only its header survives, in the INFO reply.
