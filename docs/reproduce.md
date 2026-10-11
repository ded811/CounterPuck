# Reproduce it yourself

Everything in this repo can be checked against your own Steam install and your own puck.
Valve's files never need to be shared: you already have them.

**You need:** Windows with Steam, Python 3.11 or newer (standard library only), and a
Steam Controller 2 puck for the live capture. For step 4 you also need
[Wireshark](https://www.wireshark.org/) installed with the **USBPcap** component ticked, followed by a reboot.

All commands are PowerShell, run from the root of this repo. They assume Steam is in its
default location; adjust `$hw` if yours isn't.

```powershell
$hw = 'C:\Program Files (x86)\Steam\bin\hardwareupdater'
Get-ChildItem $hw
Get-FileHash "$hw\*.fw" -Algorithm SHA256
```

Compare the hashes with [firmware-format.md](firmware-format.md). If Steam has shipped a newer
build since, the names and numbers will differ, but the structure should be the same.

## 1. Inspect the firmware file

```powershell
$fw = (Get-ChildItem "$hw\PROTEUS_FW_*.fw").FullName
python -I tools\fw_header.py $fw
```

Expect the PROTEUS magic `0x2e795631`, a payload length equal to the file size minus 32, a
matching CRC32, a stack pointer in RAM (`0x2001xxxx`), and an average entropy around
5.4 bits/byte. Together those mean plaintext code.

## 2. Find the load address

```powershell
python -I tools\find_base.py $fw
```

One base wins by a wide margin: `0x00008000` for build `6A628359`.

## 3. Look inside the updater (nothing is run)

```powershell
python -I tools\pyi_extract.py "$hw\hardwareupdater.exe"
```

This lists the PyInstaller bundle: Python 3.14, `hid`, `serial` and one script called
`hardwareupdater`. To dig further, extract it into an empty folder and read the strings of the
script's code object:

```powershell
python -I tools\pyi_extract.py "$hw\hardwareupdater.exe" private\updater-extract
```

## 4. Capture an update

Steam offers a puck update when the puck's firmware is older than the build named in
`hardwareupdater.cfg`. You need an update pending to capture one.

1. In a normal PowerShell window, start the passive logger. It records updater launches, USB
   devices appearing and disappearing, and any new firmware files. It doesn't need admin.

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\watch_update.ps1
   ```

2. In an **Administrator** PowerShell window, start the capture. It finds the root hub the
   puck is on and records only the puck, its hub and devices that connect during the capture.
   That catches the bootloader without recording your keyboard.

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\capture_usb.ps1
   ```

   It should print `Puck found` and then `CAPTURING`. If it can't find the puck, it falls back
   to capturing every device, including keyboards, and warns you.

3. Start the update in Steam. Don't unplug anything or plug anything in while it runs.

4. When Steam reports success, wait about 10 s, then press Enter in the Administrator window.
   To stop the logger, create an empty file named `STOP` in its output folder, or let it time
   out.

Both write to `private\captures\<date>_puck-update\`, which `.gitignore` keeps out of git. Keep
it that way: the capture contains Valve's full firmware image and your puck's serial numbers.

## 5. Decode the capture

```powershell
$pcap = (Get-ChildItem private\captures\*\usbpcap\*.pcap | Select-Object -Last 1).FullName
python -I tools\usbpcap_parse.py $pcap summary
```

In the summary, find the device address with a bulk OUT endpoint (`0x01`) carrying about
198 KB. That's the puck in bootloader mode, which Windows may give the same address as the
running puck. Then:

```powershell
python -I tools\usbpcap_parse.py $pcap control --dev 5     # HID feature reports, decoded
python -I tools\decode_bl_stream.py $pcap --dev 5 --fw $fw  # bootloader messages + image check
```

Replace `5` with your device address. The last lines should read
`payload identical to wire image : True` and `header identical to FW_END body : True`.

## 6. Decompile (optional, keep it local)

The payload is an ordinary Cortex-M image. In [Ghidra](https://ghidra-sre.org/), import it as a
**raw binary** with language `ARM:LE:32:Cortex` and base address `0x8000`, then run
auto-analysis. Strip the 32-byte header first, or set the file offset to `0x20`.

To export everything without the GUI, use the headless analyzer with
[`tools/GhidraExport.java`](../tools/GhidraExport.java):

```powershell
& "<ghidra>\support\analyzeHeadless.bat" "$PWD\decomp\project" proteus `
    -import "$PWD\decomp\proteus_app_0x8000.bin" `
    -processor ARM:LE:32:Cortex -loader BinaryLoader -loader-baseAddr 0x8000 `
    -scriptPath "$PWD\tools" -postScript GhidraExport.java "$PWD\decomp\out"
```

Three more headless scripts in `tools/` help with a deeper pass, each taking its arguments after
the script name like `GhidraExport.java`: `GhidraCreateFunctions.java` (a text file of addresses,
one per line, to turn into functions), `GhidraXrefDump.java` (an output folder for `xrefs.tsv`)
and `GhidraApplyNames.java` (a JSON naming table you write yourself, in the format shown at the
top of the script, and an output folder for the re-export). What these scripts write is derived
from the firmware too, so keep it under `decomp/` as well.

Decompiler output is derived from Valve's copyrighted firmware. `decomp/` is gitignored; don't
publish what's in it.
