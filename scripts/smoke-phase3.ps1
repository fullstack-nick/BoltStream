param(
  [string]$Preset = "windows-gcc-debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$Logtool = Join-Path $BuildDir "boltstream-logtool.exe"
if (-not (Test-Path $Logtool)) {
  $Logtool = Join-Path $BuildDir "boltstream-logtool"
}
if (-not (Test-Path $Logtool)) {
  throw "boltstream-logtool was not found in $BuildDir. Build preset '$Preset' first."
}

$DataDir = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase3-smoke-$([Guid]::NewGuid())"
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null

function Invoke-Logtool {
  param([string[]]$ToolArgs)

  $output = & $Logtool @ToolArgs
  if ($LASTEXITCODE -ne 0) {
    throw "boltstream-logtool failed with exit code $LASTEXITCODE for args: $($ToolArgs -join ' ')"
  }
  return ($output -join "`n")
}

try {
  $append1 = Invoke-Logtool @(
    "append", "--data", $DataDir, "--topic", "trades", "--key", "AAPL",
    "--message", "phase3-message-000000000000000000000000000000", "--segment-bytes", "96"
  ) | ConvertFrom-Json
  if ($append1.offset -ne 0 -or $append1.next_offset -ne 1) {
    throw "unexpected first append output: $($append1 | ConvertTo-Json -Compress)"
  }

  $append2 = Invoke-Logtool @(
    "append", "--data", $DataDir, "--topic", "trades", "--key", "MSFT",
    "--message", "phase3-message-111111111111111111111111111111", "--segment-bytes", "96"
  ) | ConvertFrom-Json
  if ($append2.offset -ne 1 -or $append2.next_offset -ne 2) {
    throw "unexpected second append output: $($append2 | ConvertTo-Json -Compress)"
  }

  $read = Invoke-Logtool @(
    "read", "--data", $DataDir, "--topic", "trades", "--from", "0",
    "--max-records", "10", "--segment-bytes", "96"
  ) | ConvertFrom-Json
  if ($read.count -ne 2 -or $read.records[0].key -ne "AAPL" -or $read.records[1].key -ne "MSFT") {
    throw "unexpected read output: $($read | ConvertTo-Json -Compress)"
  }

  $partitionDir = Join-Path $DataDir "topics\trades\partition-000000"
  $lastLog = Get-ChildItem -Path $partitionDir -Filter "*.log" | Sort-Object Name | Select-Object -Last 1
  if (-not $lastLog) {
    throw "no segment log was created under $partitionDir"
  }

  $garbage = [byte[]](0x12, 0x34, 0x56)
  $stream = [System.IO.File]::Open($lastLog.FullName, [System.IO.FileMode]::Append,
    [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
  try {
    $stream.Write($garbage, 0, $garbage.Length)
  } finally {
    $stream.Dispose()
  }

  $recover = Invoke-Logtool @(
    "recover", "--data", $DataDir, "--topic", "trades", "--segment-bytes", "96"
  ) | ConvertFrom-Json
  if ($recover.bytes_truncated -lt 3 -or $recover.next_offset -ne 2) {
    throw "unexpected recovery output: $($recover | ConvertTo-Json -Compress)"
  }

  $readAfterRecover = Invoke-Logtool @(
    "read", "--data", $DataDir, "--topic", "trades", "--from", "0",
    "--max-records", "10", "--segment-bytes", "96"
  ) | ConvertFrom-Json
  if ($readAfterRecover.count -ne 2 -or $readAfterRecover.records[0].message -notmatch "phase3") {
    throw "unexpected post-recovery read output: $($readAfterRecover | ConvertTo-Json -Compress)"
  }

  Write-Host "Phase 3 storage smoke passed with data dir $DataDir."
} finally {
  Remove-Item -Recurse -Force -LiteralPath $DataDir -ErrorAction SilentlyContinue
}
