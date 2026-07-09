param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [string]$ExpectedGitSha = "",
  [string]$BuildDir = "",
  [int]$TunnelPort = 19100
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing Phase 9 live smoke. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}
$ActiveProject = (& $Gcloud config get-value project 2>$null).Trim()
if ($ActiveProject -ne $ProjectId) {
  throw "Refusing Phase 9 live smoke. Active project is '$ActiveProject', expected '$ProjectId'."
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot "build\windows-gcc-debug"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot $BuildDir
}

function Get-ToolPath {
  param([string]$Name)
  foreach ($candidate in @((Join-Path $BuildDir "$Name.exe"), (Join-Path $BuildDir $Name))) {
    if (Test-Path $candidate) { return $candidate }
  }
  throw "Required binary not found: $Name"
}

function Invoke-JsonTool {
  param([string]$Tool, [string[]]$ToolArgs, [switch]$ExpectFailure)
  $output = & $Tool @ToolArgs
  $exitCode = $LASTEXITCODE
  if ($ExpectFailure -and $exitCode -eq 0) { throw "$Tool unexpectedly succeeded." }
  if (-not $ExpectFailure -and $exitCode -ne 0) {
    throw "$Tool failed with exit code ${exitCode}: $($output -join "`n")"
  }
  return (($output -join "`n") | ConvertFrom-Json)
}

function Get-LiveMetrics {
  $content = (& curl.exe -fsS "http://127.0.0.1:$TunnelPort/metrics") -join "`n"
  if ($LASTEXITCODE -ne 0) { throw "Live metrics scrape through the tunnel failed." }
  return $content
}

function Require-Metric {
  param([string]$Metrics, [string]$Pattern, [string]$Description)
  if ($Metrics -notmatch $Pattern) { throw "Missing live metric for ${Description}: $Pattern" }
}

function Invoke-RemoteScript {
  param([string]$Body, [string]$Name)
  $local = Join-Path $env:TEMP "$Name-$([Guid]::NewGuid()).sh"
  $remote = "/tmp/$Name-$([Guid]::NewGuid()).sh"
  [System.IO.File]::WriteAllText($local, $Body.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)
  try {
    & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $local "${InstanceName}:$remote" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to copy remote script $Name." }
    $output = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $remote"
    if ($LASTEXITCODE -ne 0) { throw "Remote script $Name failed: $($output -join "`n")" }
    return $output
  } finally {
    Remove-Item -Force -LiteralPath $local -ErrorAction SilentlyContinue
  }
}

$Producer = Get-ToolPath "boltstream-producer"
$Consumer = Get-ToolPath "boltstream-consumer"
$Admin = Get-ToolPath "boltstream-admin"
$TokenOutput = & $Gcloud secrets versions access latest --secret "boltstream-broker-token" --project $ProjectId 2>$null
$BrokerToken = ($TokenOutput -join "`n").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($BrokerToken)) {
  throw "Secret Manager broker token is unavailable."
}
$ExternalIp = (& $Gcloud compute instances describe $InstanceName --project $ProjectId --zone $Zone --format="value(networkInterfaces[0].accessConfigs[0].natIP)").Trim()
if ([string]::IsNullOrWhiteSpace($ExternalIp)) { throw "Live VM external IP is unavailable." }

$HadToken = Test-Path Env:\BOLTSTREAM_BROKER_TOKEN
$PreviousToken = $env:BOLTSTREAM_BROKER_TOKEN
$env:BOLTSTREAM_BROKER_TOKEN = $BrokerToken
$TunnelScript = Join-Path $PSScriptRoot "metrics-tunnel.ps1"
$TunnelOut = Join-Path $env:TEMP "boltstream-phase9-tunnel.out"
$TunnelErr = Join-Path $env:TEMP "boltstream-phase9-tunnel.err"
Remove-Item -Force -LiteralPath $TunnelOut, $TunnelErr -ErrorAction SilentlyContinue
$tunnel = $null
$PowerShellExe = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName

$CleanupScript = @'
#!/usr/bin/env bash
set -euo pipefail
sudo rm -f /etc/systemd/system/boltstream.service.d/phase9-smoke.conf
sudo rmdir /etc/systemd/system/boltstream.service.d 2>/dev/null || true
sudo systemctl daemon-reload
sudo systemctl restart boltstream.service
for i in {1..60}; do
  curl -fsS http://127.0.0.1:9100/health/ready >/dev/null && exit 0
  sleep 0.25
done
exit 1
'@

try {
  $tunnel = Start-Process -FilePath $PowerShellExe -ArgumentList @(
    "-NoProfile", "-File", $TunnelScript, "-ProjectId", $ProjectId, "-Zone", $Zone,
    "-InstanceName", $InstanceName, "-LocalPort", "$TunnelPort"
  ) -PassThru -WindowStyle Hidden -RedirectStandardOutput $TunnelOut -RedirectStandardError $TunnelErr
  $deadline = [DateTimeOffset]::UtcNow.AddSeconds(30)
  do {
    & curl.exe -fsS "http://127.0.0.1:$TunnelPort/health/ready" 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { break }
    Start-Sleep -Milliseconds 250
  } while ([DateTimeOffset]::UtcNow -lt $deadline)
  if ($LASTEXITCODE -ne 0) {
    $errorText = if (Test-Path $TunnelErr) { Get-Content -Raw $TunnelErr } else { "" }
    throw "Metrics tunnel did not become ready: $errorText"
  }

  $version = (& curl.exe -fsS "http://127.0.0.1:$TunnelPort/version") -join "`n"
  if ($ExpectedGitSha -and $version -notmatch [Regex]::Escape($ExpectedGitSha)) {
    throw "Live /version did not contain expected SHA $ExpectedGitSha."
  }
  $initial = Get-LiveMetrics
  Require-Metric $initial 'boltstream_ready 1' "readiness"
  if ($ExpectedGitSha) {
    Require-Metric $initial "boltstream_build_info\{[^\r\n]*git_sha=`"$([Regex]::Escape($ExpectedGitSha))`"" "build SHA"
  }
  Require-Metric $initial 'boltstream_storage_capacity_bytes [1-9][0-9]*' "filesystem capacity"

  $TrafficTopic = "phase9-traffic-$([DateTimeOffset]::UtcNow.ToString('yyyyMMddHHmmss'))"
  Invoke-JsonTool $Admin @("topics", "create", "--host", $ExternalIp, "--port", "9000", "--topic", $TrafficTopic, "--partitions", "1") | Out-Null
  for ($i = 0; $i -lt 150; ++$i) {
    Invoke-JsonTool $Producer @("--host", $ExternalIp, "--port", "9000", "--topic", $TrafficTopic, "--key", "$i", "--message", "live-phase9-$i") | Out-Null
  }
  $first = Invoke-JsonTool $Consumer @("--host", $ExternalIp, "--port", "9000", "--topic", $TrafficTopic, "--group", "phase9dashboard", "--from", "beginning", "--commit")
  if ($first.count -ne 100 -or $first.committed_offset -ne 100) { throw "Unexpected first live Phase 9 consume batch." }

  $trafficMetrics = Get-LiveMetrics
  Require-Metric $trafficMetrics "boltstream_consumer_group_lag_records\{group=`"phase9dashboard`",topic=`"$TrafficTopic`",partition=`"0`"\} 50" "live consumer lag"
  Require-Metric $trafficMetrics "boltstream_partition_log_bytes\{topic=`"$TrafficTopic`",partition=`"0`"\} [1-9][0-9]*" "live partition bytes"
  Require-Metric $trafficMetrics 'boltstream_request_duration_seconds_count\{operation="produce"\} [1-9][0-9]*' "produce histogram"

  $second = Invoke-JsonTool $Consumer @("--host", $ExternalIp, "--port", "9000", "--topic", $TrafficTopic, "--group", "phase9dashboard", "--commit")
  if ($second.count -ne 50 -or $second.committed_offset -ne 150) { throw "Live Phase 9 consumer did not catch up." }
  $caughtUp = Get-LiveMetrics
  Require-Metric $caughtUp "boltstream_consumer_group_lag_records\{group=`"phase9dashboard`",topic=`"$TrafficTopic`",partition=`"0`"\} 0" "zero live consumer lag"
  Invoke-JsonTool $Admin @("topics", "delete", "--host", $ExternalIp, "--port", "9000", "--topic", $TrafficTopic) | Out-Null
  $afterDelete = Get-LiveMetrics
  if ($afterDelete -match "topic=`"$([Regex]::Escape($TrafficTopic))`"") { throw "Deleted traffic topic metrics did not disappear." }

  $RetentionOverride = @'
#!/usr/bin/env bash
set -euo pipefail
sudo mkdir -p /etc/systemd/system/boltstream.service.d
sudo tee /etc/systemd/system/boltstream.service.d/phase9-smoke.conf >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=/opt/boltstream/current/bin/boltstream-server --config /etc/boltstream/boltstream.yaml --segment-bytes 96 --segment-max-age-seconds 0 --retention-max-age-seconds 1 --retention-max-bytes 0 --retention-check-interval-ms 0
EOF
sudo systemctl daemon-reload
sudo systemctl restart boltstream.service
for i in {1..60}; do curl -fsS http://127.0.0.1:9100/health/ready >/dev/null && exit 0; sleep 0.25; done
exit 1
'@
  Invoke-RemoteScript $RetentionOverride "boltstream-phase9-retention-override" | Out-Null
  $RetentionTopic = "phase9-retention-$([DateTimeOffset]::UtcNow.ToString('yyyyMMddHHmmss'))"
  Invoke-JsonTool $Admin @("topics", "create", "--host", $ExternalIp, "--port", "9000", "--topic", $RetentionTopic, "--partitions", "1") | Out-Null
  for ($i = 0; $i -lt 3; ++$i) {
    Invoke-JsonTool $Producer @("--host", $ExternalIp, "--port", "9000", "--topic", $RetentionTopic, "--key", "$i", "--message", "message-00000000000000000000000000000000") | Out-Null
  }
  $AgeScript = @"
#!/usr/bin/env bash
set -euo pipefail
PART_DIR="/var/lib/boltstream/topics/$RetentionTopic/partition-000000"
mapfile -t logs < <(find "`$PART_DIR" -maxdepth 1 -name '*.log' | sort)
test "`${#logs[@]}" -ge 3
for ((i=0; i<`${#logs[@]}-1; ++i)); do sudo touch -d '10 seconds ago' "`${logs[`$i]}"; done
"@
  Invoke-RemoteScript $AgeScript "boltstream-phase9-age" | Out-Null
  $retained = Invoke-JsonTool $Admin @("retention", "run", "--host", $ExternalIp, "--port", "9000", "--topic", $RetentionTopic)
  if ($retained.segments_deleted -lt 2) { throw "Live Phase 9 retention did not delete expected segments." }
  $retentionMetrics = Get-LiveMetrics
  Require-Metric $retentionMetrics 'boltstream_retention_deleted_segments_total [2-9][0-9]*' "retention deleted segments"
  Require-Metric $retentionMetrics 'boltstream_retention_deleted_bytes_total [1-9][0-9]*' "retention deleted bytes"
  Invoke-JsonTool $Admin @("topics", "delete", "--host", $ExternalIp, "--port", "9000", "--topic", $RetentionTopic) | Out-Null

  $OverloadOverride = @'
#!/usr/bin/env bash
set -euo pipefail
sudo mkdir -p /etc/systemd/system/boltstream.service.d
sudo tee /etc/systemd/system/boltstream.service.d/phase9-smoke.conf >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=/opt/boltstream/current/bin/boltstream-server --config /etc/boltstream/boltstream.yaml --max-append-queue-depth 0 --retention-check-interval-ms 0
EOF
sudo systemctl daemon-reload
sudo systemctl restart boltstream.service
for i in {1..60}; do curl -fsS http://127.0.0.1:9100/health/ready >/dev/null && exit 0; sleep 0.25; done
exit 1
'@
  Invoke-RemoteScript $OverloadOverride "boltstream-phase9-overload-override" | Out-Null
  $OverloadTopic = "phase9-overload-$([DateTimeOffset]::UtcNow.ToString('yyyyMMddHHmmss'))"
  Invoke-JsonTool $Admin @("topics", "create", "--host", $ExternalIp, "--port", "9000", "--topic", $OverloadTopic, "--partitions", "1") | Out-Null
  $rejected = Invoke-JsonTool $Producer @("--host", $ExternalIp, "--port", "9000", "--topic", $OverloadTopic, "--key", "blocked", "--message", "blocked") -ExpectFailure
  if ($rejected.error_code -ne "overloaded") { throw "Expected live overload error." }
  $overloadMetrics = Get-LiveMetrics
  Require-Metric $overloadMetrics 'boltstream_request_errors_total\{operation="produce",error_code="overloaded"\} 1' "live overload error"
  Require-Metric $overloadMetrics 'boltstream_rejected_requests_total\{operation="produce",reason="append_queue"\} 1' "live append rejection"
  $JournalCheck = @'
#!/usr/bin/env bash
set -euo pipefail
journalctl -u boltstream.service -o cat -n 100 --no-pager | grep '"event":"append_overloaded"' | tail -n 1
journalctl -u boltstream.service -o cat -n 100 --no-pager | grep '"error_code":"overloaded"' | tail -n 1
'@
  $journal = Invoke-RemoteScript $JournalCheck "boltstream-phase9-journal"
  if (($journal -join "`n") -match [Regex]::Escape($BrokerToken)) { throw "Broker token leaked into live journal." }
  Invoke-JsonTool $Admin @("topics", "delete", "--host", $ExternalIp, "--port", "9000", "--topic", $OverloadTopic) | Out-Null

  Invoke-RemoteScript $CleanupScript "boltstream-phase9-cleanup" | Out-Null
  $finalVersion = (& curl.exe -fsS "http://127.0.0.1:$TunnelPort/version") -join "`n"
  if ($ExpectedGitSha -and $finalVersion -notmatch [Regex]::Escape($ExpectedGitSha)) {
    throw "Final restored service SHA mismatch."
  }
  Write-Host "Phase 9 live metrics, lag, retention, overload, log, and cleanup proof passed."
} finally {
  try { Invoke-RemoteScript $CleanupScript "boltstream-phase9-final-cleanup" | Out-Null } catch { Write-Warning $_ }
  if ($tunnel -and -not $tunnel.HasExited) {
    Stop-Process -Id $tunnel.Id -Force -ErrorAction SilentlyContinue
    $tunnel.WaitForExit()
  }
  if ($HadToken) { $env:BOLTSTREAM_BROKER_TOKEN = $PreviousToken } else { Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue }
}
