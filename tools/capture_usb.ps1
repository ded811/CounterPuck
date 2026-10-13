<#
.SYNOPSIS
  Capture the USB traffic of a firmware update with USBPcap. Must run from an ELEVATED (Run as
  Administrator) PowerShell. Passive sniffing only, nothing is sent to the device.

  -Target Puck (default): finds the root hub the puck is on, then captures ONLY the puck and
  its parent hub, plus any device that connects during the capture. That last part is what
  catches the bootloader, which re-enumerates as a new USB serial device (28DE:1005/1007).
  Your keyboard and mouse on the same root hub are NOT recorded.

  -Target Controller: the controller (28DE:1302 on its USB cable) has no serial port to be found
  by, so the script asks you to plug it in and captures the device that appeared, the same way.

  -AllDevices: fallback that captures every device on every root hub (includes keystrokes).

  Don't plug anything else in while it runs (new devices get captured too).
  Press Enter in this window to stop. Wait ~10 s after Steam says the update finished.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\capture_usb.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\capture_usb.ps1 -Target Controller
#>
param(
    [ValidateSet('Puck', 'Controller')]
    [string]$Target = 'Puck',
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot) ("private\captures\{0:yyyy-MM-dd}_{1}-update" -f (Get-Date), $Target.ToLower())),
    [switch]$AllDevices
)

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw 'Run this from an elevated PowerShell (Run as Administrator).' }

$cmd = @(
    'C:\Program Files\USBPcap\USBPcapCMD.exe',
    'C:\Program Files\Wireshark\extcap\USBPcapCMD.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cmd) { throw 'USBPcapCMD.exe not found. Install Wireshark with the USBPcap component ticked, then reboot.' }

$ifaces = @(& $cmd --extcap-interfaces | Select-String -Pattern 'value=(\\\\\.\\USBPcap\d+)' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
if (-not $ifaces) { throw 'No USBPcap interfaces found. USBPcap needs a reboot after install.' }

# The device tree USBPcap shows for one root hub: key ("5", "5_1", ...) -> @{ Display; Parent }.
function Get-Tree([string]$iface) {
    $entries = @{}
    & $cmd --extcap-interface $iface --extcap-config 2>$null | ForEach-Object {
        if ($_ -match '^value \{arg=99\}\{value=([^}]+)\}\{display=([^}]*)\}.*?(?:\{parent=([^}]+)\})?$') {
            $entries[$Matches[1]] = @{ Display = $Matches[2]; Parent = $Matches[3] }
        }
    }
    $entries
}

# The USB addresses of the hubs between a device and its root hub (empty if it sits on the root hub).
function Get-HubChain($entries, [string]$dev) {
    $chain = @()
    $p = $entries[$dev].Parent
    while ($p) { $chain = @($p) + $chain; $p = $entries[$p].Parent }
    $chain
}

# Returns @{ Iface; Dev; Hubs; Label } for the root hub holding the puck's CDC serial port, or $null.
function Find-Puck {
    $serial = Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'USB\\VID_28DE&PID_1304&MI_00%'" | Select-Object -First 1
    if (-not $serial -or $serial.Name -notmatch '\((COM\d+)\)') { return $null }
    $com = $Matches[1]
    foreach ($i in $ifaces) {
        $entries = Get-Tree $i
        $hit = $entries.Keys | Where-Object { $entries[$_].Display -like "*($com)*" } | Select-Object -First 1
        if (-not $hit) { continue }
        $puck = $entries[$hit].Parent                       # e.g. "5" (the composite device)
        while ($puck -and $puck.Contains('_')) { $puck = $entries[$puck].Parent }
        if (-not $puck) { continue }
        return @{ Iface = $i; Dev = $puck; Hubs = @(Get-HubChain $entries $puck); Label = "puck ($com)" }
    }
    return $null
}

# One string per top-level device: its address and the names of everything under it. A replugged
# device can get a freed address back, so the address alone does not show what is new.
function Get-Signatures($entries) {
    foreach ($k in $entries.Keys) {
        if ($k.Contains('_')) { continue }
        $kids = @($entries.Keys | Where-Object { $_ -like "${k}_*" } | Sort-Object | ForEach-Object { $entries[$_].Display })
        "$k|$($entries[$k].Display)|$($kids -join ';')"
    }
}

# Asks for the controller to be plugged in and returns the device that appeared, like Find-Puck.
function Find-Controller {
    $before = @{}
    foreach ($i in $ifaces) { $before[$i] = @(Get-Signatures (Get-Tree $i)) }
    Write-Host ''
    Write-Host 'Plug the controller into the PC with its USB cable now (and nothing else).' -ForegroundColor Yellow
    Read-Host 'When Windows has finished setting it up, press Enter'
    for ($try = 0; $try -lt 5; $try++) {
        $pnp = Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'USB\\VID_28DE&PID_1302%'" | Select-Object -First 1
        foreach ($i in $ifaces) {
            $entries = Get-Tree $i
            $new = @(Get-Signatures $entries | Where-Object { $before[$i] -notcontains $_ } |
                    ForEach-Object { $_.Split('|')[0] } | Where-Object { $entries[$_].Display -notmatch 'Hub' })
            if ($new.Count -gt 1) { Write-Warning "More than one new device on $i ($($new -join ',')): unplug the others."; return $null }
            if ($new.Count -eq 1) {
                if (-not $pnp) { Write-Warning 'A new device appeared, but Windows lists no 28DE:1302 controller.' }
                return @{ Iface = $i; Dev = $new[0]; Hubs = @(Get-HubChain $entries $new[0]); Label = "controller, $($entries[$new[0]].Display)" }
            }
        }
        Start-Sleep -Seconds 2
    }
    return $null
}

$pcapDir = Join-Path $OutDir 'usbpcap'
New-Item -ItemType Directory -Force $pcapDir | Out-Null
Write-Host "USBPcapCMD : $cmd"
Write-Host "Output     : $pcapDir"

$common = @('--inject-descriptors', '-b', '8388608', '-s', '65535')
$jobs = @()
$target = if ($AllDevices) { $null } elseif ($Target -eq 'Controller') { Find-Controller } else { Find-Puck }
if (-not $target -and -not $AllDevices -and $Target -eq 'Controller') {
    throw 'No new controller found. Unplug it, run the script again and plug it in when asked (or use -AllDevices).'
}

if ($target) {
    $devs = (@($target.Hubs) + @($target.Dev) | Where-Object { $_ }) -join ','
    $name = ($target.Iface -replace '^.*\\', '')
    $file = Join-Path $pcapDir ("{0}_{1}_{2:HHmmss}.pcap" -f $name, $Target.ToLower(), (Get-Date))
    Write-Host "Found      : $($target.Label) on $($target.Iface), USB address $($target.Dev), hubs [$($target.Hubs -join ',')]"
    Write-Host "  capturing devices $devs + newly connected devices -> $file"
    $jobs += Start-Process -FilePath $cmd -WindowStyle Hidden -PassThru -ArgumentList (@(
        '-d', $target.Iface, '-o', "`"$file`"", '--devices', $devs, '--capture-from-new-devices') + $common)
}
else {
    if (-not $AllDevices) { Write-Warning "$Target not found on any root hub; falling back to capturing ALL devices on ALL hubs." }
    Write-Warning 'All-devices mode records keyboard traffic too. Do not type passwords while it runs.'
    foreach ($i in $ifaces) {
        $name = ($i -replace '^.*\\', '')
        $file = Join-Path $pcapDir ("{0}_all_{1:HHmmss}.pcap" -f $name, (Get-Date))
        Write-Host "  capturing $i -> $file"
        $jobs += Start-Process -FilePath $cmd -WindowStyle Hidden -PassThru -ArgumentList (@(
            '-d', $i, '-o', "`"$file`"", '-A') + $common)
    }
}

Start-Sleep -Seconds 2
$dead = @($jobs | Where-Object { $_.HasExited })
if ($dead) { Write-Warning "$($dead.Count) capture process(es) exited immediately - check USBPcap install." }

Write-Host ''
Write-Host '>>> CAPTURING. Start the update in Steam now. <<<' -ForegroundColor Green
Read-Host 'Press Enter to stop (wait ~10 s after the update finishes)'

$jobs | Where-Object { -not $_.HasExited } | Stop-Process -Force
Start-Sleep -Milliseconds 500
Get-ChildItem $pcapDir -Filter *.pcap | Select-Object Name, Length | Format-Table -AutoSize
