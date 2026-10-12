<#
.SYNOPSIS
  Build Counterpuck for the SuperMini / Pro Micro nRF52840 (UF2 bootloader). Needs a STOCK
  nRF Connect SDK v2.9.0 (no patches). Without `west` on PATH, the toolchain is loaded from C:\ncs.
.EXAMPLE
  .\build.ps1                       # -> build\counterpuck\zephyr\zephyr.uf2
  .\build.ps1 -Pristine
  .\build.ps1 -OutDir C:\b          # a short folder, if the paths get too long for Windows
#>
param(
    [string]$Board = 'promicro_nrf52840/nrf52840/uf2',
    [string]$OutDir = (Join-Path $PSScriptRoot 'build'),
    [switch]$Pristine
)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command west -ErrorAction SilentlyContinue)) {
    . (Join-Path $PSScriptRoot 'ncs_env.ps1')
}
$p = if ($Pristine) { 'always' } else { 'auto' }
west build -b $Board -p $p -d $OutDir $PSScriptRoot -- "-DBOARD_ROOT=$($PSScriptRoot -replace '\\', '/')"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (Test-Path (Join-Path $OutDir 'partitions.yml')) {
    throw 'Partition Manager output found (partitions.yml): sysbuild.conf is missing or stale; use -Pristine.'
}
$app = Join-Path $OutDir (Split-Path $PSScriptRoot -Leaf)
if (-not (Test-Path (Join-Path $app 'zephyr'))) { $app = $OutDir }
$uf2 = Join-Path $app 'zephyr/zephyr.uf2'
if (Test-Path $uf2) {
    Write-Host "`nUF2 ready: $uf2" -ForegroundColor Green
    Write-Host 'Double-tap RST on the board, then copy the .uf2 onto the drive that appears.'
}
