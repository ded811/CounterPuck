<#
.SYNOPSIS
  Load the nRF Connect SDK v2.9.0 toolchain environment into the current PowerShell session, the
  same way Toolchain Manager's terminal does (toolchains\<bundle>\environment.json), plus ZEPHYR_BASE.
  Dot-source it:  . .\ncs_env.ps1     (build.ps1 in both apps does this when west is not on PATH)
#>
param(
    [string]$NcsRoot = 'C:\ncs',
    [string]$NcsVersion = 'v2.9.0'
)
$ErrorActionPreference = 'Stop'
$index = Join-Path $NcsRoot 'toolchains\toolchains.json'
if (-not (Test-Path $index)) { throw "No toolchain index at $index. Install NCS $NcsVersion with Toolchain Manager first." }
$bundle = (Get-Content $index -Raw | ConvertFrom-Json) |
    ForEach-Object { $_.toolchains } |
    Where-Object { $_.ncs_versions -contains $NcsVersion } |
    Select-Object -First 1
if (-not $bundle) { throw "No toolchain for $NcsVersion listed in $index." }
$tc = Join-Path $NcsRoot ("toolchains\" + $bundle.identifier.bundle_id)
$envDef = Get-Content (Join-Path $tc 'environment.json') -Raw | ConvertFrom-Json
foreach ($v in $envDef.env_vars) {
    if ($v.type -eq 'string') {
        $value = $v.value
    } else {
        $value = ($v.values | ForEach-Object { Join-Path $tc $_ }) -join ';'
    }
    if ($v.existing_value_treatment -eq 'prepend_to' -and (Get-Item "env:$($v.key)" -ErrorAction SilentlyContinue)) {
        $value = $value + ';' + (Get-Item "env:$($v.key)").Value
    }
    Set-Item "env:$($v.key)" $value
}
$env:ZEPHYR_BASE = Join-Path $NcsRoot "$NcsVersion\zephyr"
if (-not (Test-Path $env:ZEPHYR_BASE)) { throw "SDK not found at $env:ZEPHYR_BASE." }
Write-Host "NCS $NcsVersion environment loaded (toolchain $($bundle.identifier.bundle_id), ZEPHYR_BASE=$env:ZEPHYR_BASE)"
