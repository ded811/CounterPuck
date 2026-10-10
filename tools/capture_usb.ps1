<#
.SYNOPSIS
  Capture the puck's USB traffic with USBPcap. Must run from an ELEVATED (Run as
  Administrator) PowerShell. Passive sniffing only, nothing is sent to the puck.

  Default (targeted): finds the root hub the puck is on, then captures ONLY the puck and
  its parent hub, plus any device that connects during the capture. That last part is what
  catches the bootloader, which re-enumerates as a new USB serial device (28DE:1005/1007).
  Your keyboard and mouse on the same root hub are NOT recorded.

  -AllDevices: fallback that captures every device on every root hub (includes keystrokes).

  Don't plug anything else in while it runs (new devices get captured too).
  Press Enter in this window to stop. Wait ~10 s after Steam says the update finished.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\capture_usb.ps1
#>
param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot) ("private\captures\{0:yyyy-MM-dd}_puck-update" -f (Get-Date))),
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

# Returns @{ Iface; Puck; Hub } for the root hub holding the puck's CDC serial port, or $null.
function Find-Puck {
    $serial = Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'USB\\VID_28DE&PID_1304&MI_00%'" | Select-Object -First 1
    if (-not $serial -or $serial.Name -notmatch '\((COM\d+)\)') { return $null }
    $com = $Matches[1]
    foreach ($i in $ifaces) {
        $entries = @{}
        & $cmd --extcap-interface $i --extcap-config 2>$null | ForEach-Object {
            if ($_ -match '^value \{arg=99\}\{value=([^}]+)\}\{display=([^}]*)\}.*?(?:\{parent=([^}]+)\})?$') {
                $entries[$Matches[1]] = @{ Display = $Matches[2]; Parent = $Matches[3] }
            }
        }
        $hit = $entries.Keys | Where-Object { $entries[$_].Display -like "*($com)*" } | Select-Object -First 1
        if (-not $hit) { continue }
        $puck = $entries[$hit].Parent                       # e.g. "5" (the composite device)
        while ($puck -and $puck.Contains('_')) { $puck = $entries[$puck].Parent }
        $hub = if ($puck) { $entries[$puck].Parent } else { $null }   # e.g. "3", or empty if on the root hub directly
        return @{ Iface = $i; Puck = $puck; Hub = $hub; Com = $com }
    }
    return $null
}

$pcapDir = Join-Path $OutDir 'usbpcap'
New-Item -ItemType Directory -Force $pcapDir | Out-Null
Write-Host "USBPcapCMD : $cmd"
Write-Host "Output     : $pcapDir"

$common = @('--inject-descriptors', '-b', '8388608', '-s', '65535')
$jobs = @()
$target = if ($AllDevices) { $null } else { Find-Puck }

if ($target) {
    $devs = (@($target.Hub, $target.Puck) | Where-Object { $_ }) -join ','
    $name = ($target.Iface -replace '^.*\\', '')
    $file = Join-Path $pcapDir ("{0}_puck_{1:HHmmss}.pcap" -f $name, (Get-Date))
    Write-Host "Puck found : $($target.Iface), USB address $($target.Puck), hub $($target.Hub), $($target.Com)"
    Write-Host "  capturing devices $devs + newly connected devices -> $file"
    $jobs += Start-Process -FilePath $cmd -WindowStyle Hidden -PassThru -ArgumentList (@(
        '-d', $target.Iface, '-o', "`"$file`"", '--devices', $devs, '--capture-from-new-devices') + $common)
}
else {
    if (-not $AllDevices) { Write-Warning 'Puck not found on any root hub; falling back to capturing ALL devices on ALL hubs.' }
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
