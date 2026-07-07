param(
  [string]$Preset = "windows-gcc-debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-CMakePreset {
  param([string]$ConfigurePreset)

  $buildPreset = "build-$ConfigurePreset"
  cmake --preset $ConfigurePreset
  cmake --build --preset $buildPreset
}

if ($Preset.StartsWith("windows-msvc")) {
  $vcvars = (Get-Command vcvars64.bat -ErrorAction Stop).Source
  $buildPreset = "build-$Preset"
  $cmd = "call `"$vcvars`" >nul && cmake --preset $Preset && cmake --build --preset $buildPreset"
  cmd.exe /d /s /c $cmd
  exit $LASTEXITCODE
}

Invoke-CMakePreset -ConfigurePreset $Preset

