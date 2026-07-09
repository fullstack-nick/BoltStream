param(
  [string]$Preset = "windows-gcc-debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-FreePort {
  $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
  $listener.Start()
  try { return $listener.LocalEndpoint.Port } finally { $listener.Stop() }
}

function Get-ToolPath {
  param([string]$BuildDir, [string]$Name)
  foreach ($candidate in @((Join-Path $BuildDir "$Name.exe"), (Join-Path $BuildDir $Name))) {
    if (Test-Path $candidate) { return $candidate }
  }
  throw "Required binary not found: $Name"
}

function Wait-Ready {
  param([int]$AdminPort)
  $deadline = [DateTimeOffset]::UtcNow.AddSeconds(15)
  while ([DateTimeOffset]::UtcNow -lt $deadline) {
    & curl.exe -fsS "http://127.0.0.1:$AdminPort/health/ready" 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { return }
    Start-Sleep -Milliseconds 200
  }
  throw "Broker did not become ready on admin port $AdminPort."
}

function Stop-Server {
  param([object]$Process)
  if ($Process -and -not $Process.HasExited) {
    Stop-Process -Id $Process.Id -Force
    $Process.WaitForExit()
  }
}

function Invoke-JsonTool {
  param([string]$Tool, [string[]]$ToolArgs, [switch]$ExpectFailure)
  $output = & $Tool @ToolArgs
  $exitCode = $LASTEXITCODE
  if ($ExpectFailure -and $exitCode -eq 0) {
    throw "$Tool unexpectedly succeeded: $($output -join "`n")"
  }
  if (-not $ExpectFailure -and $exitCode -ne 0) {
    throw "$Tool failed with exit code ${exitCode}: $($output -join "`n")"
  }
  return (($output -join "`n") | ConvertFrom-Json)
}

function Require-Metric {
  param([string]$Metrics, [string]$Pattern, [string]$Description)
  if ($Metrics -notmatch $Pattern) {
    throw "Missing or unexpected metric for ${Description}: $Pattern"
  }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$Config = Join-Path $RepoRoot "config\boltstream.example.yaml"
$Server = Get-ToolPath $BuildDir "boltstream-server"
$Admin = Get-ToolPath $BuildDir "boltstream-admin"
$Producer = Get-ToolPath $BuildDir "boltstream-producer"
$Consumer = Get-ToolPath $BuildDir "boltstream-consumer"

& $Server --config $Config --check-config | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Checked-in config failed validation." }
$effective = & $Server --config $Config --print-effective-config --port 19090
if ($LASTEXITCODE -ne 0 -or ($effective -join "`n") -notmatch 'listen: "0\.0\.0\.0:19090"') {
  throw "Effective config did not apply the CLI override."
}

$HadToken = Test-Path Env:\BOLTSTREAM_BROKER_TOKEN
$PreviousToken = $env:BOLTSTREAM_BROKER_TOKEN
$env:BOLTSTREAM_BROKER_TOKEN = "phase9-smoke-$([Guid]::NewGuid())"
$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase9-smoke-$([Guid]::NewGuid())"
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
$DataDir = Join-Path $TempRoot "data"
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
$serverProcess = $null

try {
  $BrokerPort = Get-FreePort
  $AdminPort = Get-FreePort
  $Stdout = Join-Path $TempRoot "server.out"
  $Stderr = Join-Path $TempRoot "server.err"
  $serverProcess = Start-Process -FilePath $Server -ArgumentList @(
    "--config", $Config,
    "--listen", "127.0.0.1:$BrokerPort",
    "--admin-listen", "127.0.0.1:$AdminPort",
    "--data", $DataDir,
    "--max-fetch-records", "2",
    "--retention-check-interval-ms", "0"
  ) -PassThru -WindowStyle Hidden -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr
  Wait-Ready $AdminPort

  $topic = "phase9"
  Invoke-JsonTool $Admin @("topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic, "--partitions", "1") | Out-Null
  for ($i = 0; $i -lt 5; ++$i) {
    Invoke-JsonTool $Producer @("--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic, "--key", "$i", "--message", "metric-$i") | Out-Null
  }
  $consumed = Invoke-JsonTool $Consumer @("--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic, "--group", "dashboard", "--from", "beginning", "--commit")
  if ($consumed.count -ne 2 -or $consumed.committed_offset -ne 2) {
    throw "Expected a two-record first batch and committed offset 2."
  }

  $metrics = (& curl.exe -fsS "http://127.0.0.1:$AdminPort/metrics") -join "`n"
  if ($LASTEXITCODE -ne 0) { throw "Metrics scrape failed." }
  Require-Metric $metrics 'boltstream_build_info\{[^\r\n]*protocol_version="4"' "build info"
  Require-Metric $metrics 'boltstream_ready 1' "readiness"
  Require-Metric $metrics 'boltstream_requests_total\{operation="produce"\} 5' "produce requests"
  Require-Metric $metrics 'boltstream_records_produced_total 5' "produced records"
  Require-Metric $metrics 'boltstream_records_fetched_total 2' "fetched records"
  Require-Metric $metrics 'boltstream_consumer_group_lag_records\{group="dashboard",topic="phase9",partition="0"\} 3' "group lag"
  Require-Metric $metrics 'boltstream_partition_log_bytes\{topic="phase9",partition="0"\} [1-9][0-9]*' "partition bytes"
  Require-Metric $metrics 'boltstream_metrics_scrapes_total 1' "scrape count"

  $methodStatus = & curl.exe -sS -o NUL -w "%{http_code}" -X POST "http://127.0.0.1:$AdminPort/health/live"
  if ($methodStatus -ne "405") { throw "Expected HTTP 405 for POST, got $methodStatus." }

  Stop-Server $serverProcess
  $serverProcess = $null

  $OverloadPort = Get-FreePort
  $OverloadAdminPort = Get-FreePort
  $OverloadErr = Join-Path $TempRoot "overload.err"
  $OverloadOut = Join-Path $TempRoot "overload.out"
  $serverProcess = Start-Process -FilePath $Server -ArgumentList @(
    "--config", $Config,
    "--listen", "127.0.0.1:$OverloadPort",
    "--admin-listen", "127.0.0.1:$OverloadAdminPort",
    "--data", $DataDir,
    "--max-append-queue-depth", "0",
    "--retention-check-interval-ms", "0"
  ) -PassThru -WindowStyle Hidden -RedirectStandardOutput $OverloadOut -RedirectStandardError $OverloadErr
  Wait-Ready $OverloadAdminPort
  $rejected = Invoke-JsonTool $Producer @("--host", "127.0.0.1", "--port", "$OverloadPort", "--topic", $topic, "--key", "overload", "--message", "blocked") -ExpectFailure
  if ($rejected.error_code -ne "overloaded") { throw "Expected overloaded rejection." }
  $overloadMetrics = (& curl.exe -fsS "http://127.0.0.1:$OverloadAdminPort/metrics") -join "`n"
  Require-Metric $overloadMetrics 'boltstream_request_errors_total\{operation="produce",error_code="overloaded"\} 1' "overload error"
  Require-Metric $overloadMetrics 'boltstream_rejected_requests_total\{operation="produce",reason="append_queue"\} 1' "append rejection"

  foreach ($logPath in @($Stderr, $OverloadErr)) {
    foreach ($line in Get-Content -LiteralPath $logPath) {
      if ([string]::IsNullOrWhiteSpace($line)) { continue }
      $entry = $line | ConvertFrom-Json
      if (-not $entry.timestamp -or -not $entry.level -or -not $entry.event -or -not $entry.component -or -not $entry.git_sha) {
        throw "Structured log is missing required fields: $line"
      }
      if ($line -match [Regex]::Escape($env:BOLTSTREAM_BROKER_TOKEN)) {
        throw "Broker token leaked into structured logs."
      }
    }
  }

  Write-Host "Phase 9 metrics and operations smoke passed."
} finally {
  Stop-Server $serverProcess
  Remove-Item -Recurse -Force -LiteralPath $TempRoot -ErrorAction SilentlyContinue
  if ($HadToken) { $env:BOLTSTREAM_BROKER_TOKEN = $PreviousToken } else { Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue }
}
