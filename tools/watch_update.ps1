<#
.SYNOPSIS
  Non-admin side-channel logger for a Steam puck firmware update. Passive: it only
  observes the PC, it never talks to the puck.

  Logs to <OutDir>\watcher.log:
    - hardwareupdater.exe launches/exits (with full command line)
    - every USB device arrival/removal (catches a bootloader re-enumerating with a new VID/PID)
    - COM port appear/disappear
    - any file change in Steam\bin\hardwareupdater (new/changed files are copied out)
  On stop, copies whatever Steam appended to its logs\*.txt during the session.

  Stops when <OutDir>\STOP exists, or after -Minutes.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\watch_update.ps1
#>
param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot) ("private\captures\{0:yyyy-MM-dd}_puck-update" -f (Get-Date))),
    [double]$Minutes = 90,
    [string]$SteamDir = 'C:\Program Files (x86)\Steam'
)

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$log = Join-Path $OutDir 'watcher.log'
$stopFile = Join-Path $OutDir 'STOP'
Remove-Item $stopFile -ErrorAction SilentlyContinue
$hwDir = Join-Path $SteamDir 'bin\hardwareupdater'
$logDir = Join-Path $SteamDir 'logs'

function Log([string]$msg) {
    $line = '{0:yyyy-MM-dd HH:mm:ss.fff}  {1}' -f (Get-Date), $msg
    Add-Content -Path $log -Value $line -Encoding UTF8
    Write-Host $line
}

function Get-UsbSet {
    $set = @{}
    Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'USB\\%'" | ForEach-Object {
        $set[$_.DeviceID] = '{0} [{1}]' -f $_.Name, $_.Status
    }
    $set
}

function Get-HwDirSet {
    $set = @{}
    Get-ChildItem $hwDir -File -ErrorAction SilentlyContinue | ForEach-Object {
        $set[$_.Name] = '{0}|{1:o}' -f $_.Length, $_.LastWriteTimeUtc
    }
    $set
}

function Get-UpdaterProcs {
    $set = @{}
    Get-CimInstance Win32_Process -Filter "Name LIKE '%hardwareupdater%'" | ForEach-Object {
        $set[[string]$_.ProcessId] = 'ppid={0} path="{1}" cmd={2}' -f $_.ParentProcessId, $_.ExecutablePath, $_.CommandLine
    }
    $set
}

function Diff-Sets($old, $new, [string]$label) {
    foreach ($k in $new.Keys) {
        if (-not $old.ContainsKey($k)) { Log "$label ADDED    $k => $($new[$k])" }
        elseif ($old[$k] -ne $new[$k]) { Log "$label CHANGED  $k => $($new[$k])" }
    }
    foreach ($k in $old.Keys) {
        if (-not $new.ContainsKey($k)) { Log "$label REMOVED  $k (was $($old[$k]))" }
    }
}

# --- baseline -------------------------------------------------------------
Log "watcher start, OutDir=$OutDir, timeout=$Minutes min. Create '$stopFile' to stop."
$usb = Get-UsbSet
$hw = Get-HwDirSet
$procs = Get-UpdaterProcs
$ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
$logOffsets = @{}
Get-ChildItem $logDir -Filter *.txt -ErrorAction SilentlyContinue | ForEach-Object { $logOffsets[$_.Name] = $_.Length }

Log "baseline COM ports: $($ports -join ', ')"
foreach ($k in ($usb.Keys | Where-Object { $_ -match 'VID_28DE' } | Sort-Object)) { Log "baseline valve-usb $k => $($usb[$k])" }
foreach ($k in ($hw.Keys | Sort-Object)) {
    $h = (Get-FileHash (Join-Path $hwDir $k) -Algorithm SHA256).Hash
    Log "baseline hwupdater $k => $($hw[$k]) sha256=$h"
}
foreach ($k in $procs.Keys) { Log "baseline updater-proc $k => $($procs[$k])" }

# --- poll loop ------------------------------------------------------------
$deadline = (Get-Date).AddMinutes($Minutes)
try {
    while ((Get-Date) -lt $deadline -and -not (Test-Path $stopFile)) {
        $p2 = Get-UpdaterProcs
        Diff-Sets $procs $p2 'updater-proc'
        $procs = $p2

        $u2 = Get-UsbSet
        Diff-Sets $usb $u2 'usb'
        $usb = $u2

        $ports2 = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
        if (($ports2 -join ',') -ne ($ports -join ',')) { Log "COM ports now: $($ports2 -join ', ')" }
        $ports = $ports2

        $h2 = Get-HwDirSet
        $changed = @($h2.Keys | Where-Object { -not $hw.ContainsKey($_) -or $hw[$_] -ne $h2[$_] })
        Diff-Sets $hw $h2 'hwupdater-file'
        if ($changed.Count) {
            $dest = Join-Path $OutDir ('hardwareupdater-changes\{0:HHmmss}' -f (Get-Date))
            New-Item -ItemType Directory -Force $dest | Out-Null
            foreach ($f in $changed) {
                Copy-Item (Join-Path $hwDir $f) $dest -ErrorAction SilentlyContinue
                $h = (Get-FileHash (Join-Path $dest $f) -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
                Log "copied $f -> $dest sha256=$h"
            }
        }
        $hw = $h2

        Start-Sleep -Milliseconds 500
    }
}
finally {
    # Save whatever Steam appended to its logs during the session.
    $tailDir = Join-Path $OutDir 'steam-log-tails'
    New-Item -ItemType Directory -Force $tailDir | Out-Null
    Get-ChildItem $logDir -Filter *.txt -ErrorAction SilentlyContinue | ForEach-Object {
        $start = if ($logOffsets.ContainsKey($_.Name)) { $logOffsets[$_.Name] } else { 0 }
        if ($_.Length -gt $start) {
            $fs = [System.IO.File]::Open($_.FullName, 'Open', 'Read', 'ReadWrite')
            try {
                $null = $fs.Seek($start, 'Begin')
                $buf = New-Object byte[] ($_.Length - $start)
                $n = $fs.Read($buf, 0, $buf.Length)
                [System.IO.File]::WriteAllBytes((Join-Path $tailDir $_.Name), $buf[0..($n - 1)])
                Log "saved $n new bytes of logs\$($_.Name)"
            }
            finally { $fs.Close() }
        }
    }
    Log 'watcher stop'
}
