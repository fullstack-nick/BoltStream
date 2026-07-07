param(
  [string]$Preset = "windows-gcc-debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$testPreset = "test-$Preset"
if ($Preset.StartsWith("windows-msvc")) {
  $vcvars = (Get-Command vcvars64.bat -ErrorAction Stop).Source
  $cmd = "call `"$vcvars`" >nul && ctest --preset $testPreset"
  cmd.exe /d /s /c $cmd
  exit $LASTEXITCODE
}

ctest --preset $testPreset

