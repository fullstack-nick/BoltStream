param(
  [string]$Preset = "windows-gcc-debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "smoke-phase4.ps1") -Preset $Preset
exit $LASTEXITCODE
