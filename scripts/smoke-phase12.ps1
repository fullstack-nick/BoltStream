param(
  [string]$BuildDir = "build-phase12",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildPath = Join-Path $RepoRoot $BuildDir
$Executable = Join-Path $BuildPath "boltstream-replication-sim.exe"
if (-not (Test-Path $Executable)) {
  $Executable = Join-Path $BuildPath "boltstream-replication-sim"
}

if (-not $SkipBuild) {
  cmake -S $RepoRoot -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DBOLTSTREAM_BUILD_TESTS=ON -DBOLTSTREAM_BUILD_BENCHMARKS=OFF
  if ($LASTEXITCODE -ne 0) { throw "Phase 12 configure failed." }
  cmake --build $BuildPath --target boltstream-replication-sim boltstream_tests
  if ($LASTEXITCODE -ne 0) { throw "Phase 12 build failed." }
}

if (-not (Test-Path $Executable)) { throw "Missing Phase 12 simulator: $Executable" }

$SmokeRoot = Join-Path $BuildPath "phase12-smoke"
$Output = & $Executable --root $SmokeRoot --compression zstd 2>&1
if ($LASTEXITCODE -ne 0) { throw "Phase 12 simulator failed.`n$($Output -join "`n")" }
$Text = $Output -join "`n"
foreach ($Required in @(
  '"status":"ok"',
  '"all_timeout_observed":true',
  '"records_exact":true',
  '"lag_records":0',
  'boltstream_replication_lag_records'
)) {
  if (-not $Text.Contains($Required)) { throw "Missing Phase 12 proof: $Required" }
}

ctest --test-dir $BuildPath --output-on-failure -R ReplicationSimulationTests
if ($LASTEXITCODE -ne 0) { throw "Phase 12 replication tests failed." }

Write-Host "Phase 12 replication simulation smoke passed."
