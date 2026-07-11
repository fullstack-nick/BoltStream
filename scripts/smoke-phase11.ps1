param(
  [string]$Preset = "windows-gcc-debug",
  [switch]$SkipBuild,
  [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root "build/$Preset"
if (-not $SkipBuild) {
  cmake --build --preset "build-$Preset"
  if ($LASTEXITCODE -ne 0) { throw "Phase 11 build failed." }
}

$Suffix = Get-Random -Minimum 20000 -Maximum 40000
$BrokerPort = $Suffix
$AdminPort = $Suffix + 1
$DataDir = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase11-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $DataDir | Out-Null
$Server = Join-Path $BuildDir "boltstream-server.exe"
if (-not (Test-Path $Server)) { $Server = Join-Path $BuildDir "boltstream-server" }
$Stdout = Join-Path $DataDir "server.out"
$Stderr = Join-Path $DataDir "server.err"
$Process = Start-Process -FilePath $Server -ArgumentList @(
  "--listen", "127.0.0.1:$BrokerPort", "--admin-listen", "127.0.0.1:$AdminPort",
  "--data", $DataDir, "--max-fetch-records", "1024"
) -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr -WindowStyle Hidden -PassThru

try {
  $Ready = $false
  for ($Attempt = 0; $Attempt -lt 80; $Attempt++) {
    $Health = & curl.exe -fsS "http://127.0.0.1:$AdminPort/health/ready" 2>$null
    if ($LASTEXITCODE -eq 0) { $Ready = $true; break }
    Start-Sleep -Milliseconds 100
  }
  if (-not $Ready) { throw "Phase 11 broker did not become ready.`n$(Get-Content $Stderr -Raw)" }

  $Admin = Join-Path $BuildDir "boltstream-admin.exe"
  $Producer = Join-Path $BuildDir "boltstream-producer.exe"
  $Consumer = Join-Path $BuildDir "boltstream-consumer.exe"
  if (-not (Test-Path $Admin)) { $Admin = Join-Path $BuildDir "boltstream-admin" }
  if (-not (Test-Path $Producer)) { $Producer = Join-Path $BuildDir "boltstream-producer" }
  if (-not (Test-Path $Consumer)) { $Consumer = Join-Path $BuildDir "boltstream-consumer" }

  foreach ($Topic in @("phase11-none", "phase11-zstd")) {
    & $Admin topics create --host 127.0.0.1 --port $BrokerPort --topic $Topic --partitions 1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to create $Topic." }
  }
  $Payload = "ABCDEFGHIJKLMNOPQRSTUVWXYZ" * 10
  $NoneTimer = [System.Diagnostics.Stopwatch]::StartNew()
  $None = & $Producer --host 127.0.0.1 --port $BrokerPort --topic phase11-none `
    --message $Payload --batch-records 32 --compression none | ConvertFrom-Json
  $NoneTimer.Stop()
  $ZstdTimer = [System.Diagnostics.Stopwatch]::StartNew()
  $Zstd = & $Producer --host 127.0.0.1 --port $BrokerPort --topic phase11-zstd `
    --message $Payload --batch-records 32 --compression zstd --zstd-level 3 | ConvertFrom-Json
  $ZstdTimer.Stop()
  if ($None.record_count -ne 32 -or $Zstd.record_count -ne 32) { throw "Batch counts were not preserved." }
  if ($Zstd.encoded_bytes -ge $None.encoded_bytes) { throw "zstd did not reduce encoded bytes." }

  $NoneFetchTimer = [System.Diagnostics.Stopwatch]::StartNew()
  $NoneFetched = & $Consumer --host 127.0.0.1 --port $BrokerPort --topic phase11-none `
    --partition 0 --from beginning --compression none | ConvertFrom-Json
  $NoneFetchTimer.Stop()
  $ZstdFetchTimer = [System.Diagnostics.Stopwatch]::StartNew()
  $Fetched = & $Consumer --host 127.0.0.1 --port $BrokerPort --topic phase11-zstd `
    --partition 0 --from beginning --compression zstd | ConvertFrom-Json
  $ZstdFetchTimer.Stop()
  if ($Fetched.count -ne 32 -or $Fetched.next_offset -ne 32) { throw "Compressed fetch mismatch." }
  $Metrics = (& curl.exe -fsS "http://127.0.0.1:$AdminPort/metrics") -join "`n"
  foreach ($Required in @(
      'boltstream_compression_batches_total{codec="zstd"} 1',
      'boltstream_compressed_fetch_passthrough_total 1')) {
    if (-not $Metrics.Contains($Required)) { throw "Missing Phase 11 metric: $Required" }
  }

  $NoneBytes = (Get-Item (Join-Path $DataDir "topics/phase11-none/partition-000000/00000000000000000000.log")).Length
  $ZstdBytes = (Get-Item (Join-Path $DataDir "topics/phase11-zstd/partition-000000/00000000000000000000.log")).Length
  if ($ZstdBytes -ge $NoneBytes) { throw "zstd partition log was not smaller than none." }
  if ($JsonOut) {
    $Result = [ordered]@{
      schema_version = 2
      evidence = "single-pair-functional-smoke"
      generated_at_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
      preset = $Preset
      batch_records = 32
      payload_bytes = $Payload.Length
      zstd_level = 3
      results = @(
        [ordered]@{ codec = "none"; records = 32; logical_bytes = $None.logical_bytes;
          encoded_bytes = $None.encoded_bytes; partition_log_bytes = $NoneBytes;
          produce_batch_latency_ms = $NoneTimer.Elapsed.TotalMilliseconds;
          produce_records_per_second = 32 / $NoneTimer.Elapsed.TotalSeconds;
          fetch_records_per_second = 32 / $NoneFetchTimer.Elapsed.TotalSeconds },
        [ordered]@{ codec = "zstd"; records = 32; logical_bytes = $Zstd.logical_bytes;
          encoded_bytes = $Zstd.encoded_bytes; partition_log_bytes = $ZstdBytes;
          produce_batch_latency_ms = $ZstdTimer.Elapsed.TotalMilliseconds;
          produce_records_per_second = 32 / $ZstdTimer.Elapsed.TotalSeconds;
          fetch_records_per_second = 32 / $ZstdFetchTimer.Elapsed.TotalSeconds }
      )
    }
    $Parent = Split-Path -Parent $JsonOut
    if ($Parent) { New-Item -ItemType Directory -Path $Parent -Force | Out-Null }
    $Result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $JsonOut -Encoding utf8
  }
  Write-Host "Phase 11 compression smoke passed: none=$NoneBytes bytes zstd=$ZstdBytes bytes."
} finally {
  Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $DataDir -Recurse -Force -ErrorAction SilentlyContinue
}
