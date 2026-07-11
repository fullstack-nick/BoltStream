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
$Logtool = Join-Path $BuildDir "boltstream-logtool$ExecutableSuffix"
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

$PreloadDir = Join-Path ([System.IO.Path]::GetTempPath()) ("boltstream-phase10-preload-" + [guid]::NewGuid())
try {
  $Prepared = (& $Bench prepare-fetch --data-dir $PreloadDir --topic phase10-preload `
      --partitions 4 --messages 17 --payload-bytes 32 --key-bytes 16 `
      --preload-batch-records 8) -join "`n"
  if ($LASTEXITCODE -ne 0 -or $Prepared -notmatch '"status":"prepared"' -or
      $Prepared -notmatch '"records":17') {
    throw "boltstream-bench direct fetch preparation failed."
  }
  $ExpectedCounts = @(5, 4, 4, 4)
  for ($Partition = 0; $Partition -lt 4; $Partition++) {
    $Read = (& $Logtool read --data $PreloadDir --topic phase10-preload --partition $Partition `
        --from 0 --max-records 32) -join "`n" | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $Read.count -ne $ExpectedCounts[$Partition] -or
        $Read.next_offset -ne $ExpectedCounts[$Partition]) {
      throw "Prepared fetch partition $Partition has the wrong record count or offsets."
    }
  }
  $PreviousErrorAction = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  & $Bench prepare-fetch --data-dir $PreloadDir --topic phase10-preload --partitions 4 `
      --messages 17 --payload-bytes 32 --key-bytes 16 2>$null | Out-Null
  $NonEmptyExit = $LASTEXITCODE
  $ErrorActionPreference = $PreviousErrorAction
  if ($NonEmptyExit -ne 3) { throw "prepare-fetch must reject non-empty partitions with exit 3." }
} finally {
  Remove-Item -Recurse -Force -LiteralPath $PreloadDir -ErrorAction SilentlyContinue
}

& (Join-Path $BuildDir "boltstream-microbench$ExecutableSuffix") --benchmark_dry_run | Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "boltstream-microbench dry run failed."
}

Write-Host "Phase 10 benchmarking and performance smoke passed."
