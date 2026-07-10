param(
  [string]$Preset = "windows-msvc-debug",
  [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& "$PSScriptRoot\bench.ps1" -Preset $Preset -Quick -SkipBuild:$SkipBuild
$ExecutableSuffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
$BuildDir = Join-Path (Get-Location) "build\$Preset"
$Bench = Join-Path $BuildDir "boltstream-bench$ExecutableSuffix"
$DryRun = (& $Bench --dry-run) -join "`n"
if ($LASTEXITCODE -ne 0 -or $DryRun -notmatch '"status":"dry_run"' -or
    $DryRun -notmatch '"published_numbers":false') {
  throw "boltstream-bench dry run was not measurement-free."
}
$PreviousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $Bench 2>$null | Out-Null
$UsageExit = $LASTEXITCODE
& $Bench run --workload invalid 2>$null | Out-Null
$InvalidExit = $LASTEXITCODE
$ErrorActionPreference = $PreviousErrorAction
if ($UsageExit -ne 2) { throw "boltstream-bench usage errors must exit 2." }
if ($InvalidExit -ne 2) { throw "boltstream-bench invalid workloads must exit 2." }

& (Join-Path $BuildDir "boltstream-microbench$ExecutableSuffix") --benchmark_dry_run | Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "boltstream-microbench dry run failed."
}

Write-Host "Phase 10 benchmarking and performance smoke passed."
