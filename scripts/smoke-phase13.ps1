param(
  [string]$BuildDir = "build-phase13",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildPath = Join-Path $RepoRoot $BuildDir
$Executable = Join-Path $BuildPath "boltstream-recovery-proof.exe"
if (-not (Test-Path $Executable)) {
  $Executable = Join-Path $BuildPath "boltstream-recovery-proof"
}

if (-not $SkipBuild) {
  cmake -S $RepoRoot -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DBOLTSTREAM_BUILD_TESTS=ON -DBOLTSTREAM_BUILD_BENCHMARKS=OFF
  if ($LASTEXITCODE -ne 0) { throw "Phase 13 configure failed." }
  cmake --build $BuildPath --target boltstream-recovery-proof boltstream_tests
  if ($LASTEXITCODE -ne 0) { throw "Phase 13 build failed." }
}

if (-not (Test-Path $Executable)) { throw "Missing Phase 13 proof executable: $Executable" }

$SmokeRoot = Join-Path $BuildPath "phase13-smoke"
$Output = & $Executable --root $SmokeRoot 2>&1
if ($LASTEXITCODE -ne 0) { throw "Phase 13 proof failed.`n$($Output -join "`n")" }
$Text = $Output -join "`n"
foreach ($Required in @(
  '"scenario":"torn-record","worker_crashed":true',
  '"scenario":"partial-batch","worker_crashed":true',
  '"scenario":"stale-index","worker_crashed":true',
  '"records_recovered":3,"next_offset":3',
  '"indexes_rebuilt":1',
  '"status":"ok"',
  '"records_exact":true'
)) {
  if (-not $Text.Contains($Required)) { throw "Missing Phase 13 proof: $Required" }
}

ctest --test-dir $BuildPath --output-on-failure -R RecoveryProofTests
if ($LASTEXITCODE -ne 0) { throw "Phase 13 focused tests failed." }

if (Test-Path $SmokeRoot) { throw "Phase 13 smoke did not clean its state: $SmokeRoot" }
Write-Host $Text
Write-Host "Phase 13 crash recovery smoke passed."
