param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [string]$ExpectedGitSha = "",
  [string]$BuildDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing live smoke. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$ExternalIp = (& $Gcloud compute instances describe $InstanceName --project $ProjectId --zone $Zone --format="value(networkInterfaces[0].accessConfigs[0].natIP)").Trim()
if (-not $ExternalIp) {
  throw "Could not determine VM external IP."
}

$BrokerTokenOutput = & $Gcloud secrets versions access latest --secret "boltstream-broker-token" --project $ProjectId 2>$null
$BrokerToken = ($BrokerTokenOutput -join "`n").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($BrokerToken)) {
  throw "Secret Manager secret 'boltstream-broker-token' must have a non-empty latest version for live smoke."
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot "build\windows-gcc-debug"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot $BuildDir
}

function Get-ToolPath {
  param(
    [Parameter(Mandatory = $true)][string]$Name
  )

  $exe = Join-Path $BuildDir "$Name.exe"
  if (Test-Path $exe) { return $exe }
  $plain = Join-Path $BuildDir $Name
  if (Test-Path $plain) { return $plain }
  throw "Required binary not found: $exe"
}

$Producer = Get-ToolPath -Name "boltstream-producer"
$Consumer = Get-ToolPath -Name "boltstream-consumer"

Write-Host "Checking TCP broker port 9000 from operator machine."
$client = [System.Net.Sockets.TcpClient]::new()
$async = $client.BeginConnect($ExternalIp, 9000, $null, $null)
if (-not $async.AsyncWaitHandle.WaitOne([TimeSpan]::FromSeconds(10))) {
  $client.Close()
  throw "Timed out connecting to broker port 9000."
}
$client.EndConnect($async)
$client.Close()
Write-Host "TCP broker port 9000 reachable."

$Topic = "live-phase4-$([DateTimeOffset]::UtcNow.ToString('yyyyMMddHHmmss'))"
$ProduceOutput = & $Producer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Topic --key AAPL --message "AAPL,100,192.41"
if ($LASTEXITCODE -ne 0) {
  throw "Live producer failed with exit code $LASTEXITCODE. Output: $($ProduceOutput -join "`n")"
}
$Produced = ($ProduceOutput -join "`n") | ConvertFrom-Json
if ($Produced.status -ne "ok" -or $Produced.offset -ne 0 -or $Produced.next_offset -ne 1) {
  throw "Unexpected live producer output: $($Produced | ConvertTo-Json -Compress)"
}

$ConsumerOutput = & $Consumer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Topic --from beginning
if ($LASTEXITCODE -ne 0) {
  throw "Live consumer failed with exit code $LASTEXITCODE. Output: $($ConsumerOutput -join "`n")"
}
$Consumed = ($ConsumerOutput -join "`n") | ConvertFrom-Json
if ($Consumed.status -ne "ok" -or $Consumed.count -ne 1 -or
    $Consumed.records[0].key -ne "AAPL" -or
    $Consumed.records[0].message -ne "AAPL,100,192.41") {
  throw "Unexpected live consumer output: $($Consumed | ConvertTo-Json -Compress)"
}
Write-Host "Live broker produce/fetch succeeded for topic $Topic."

$RemoteScript = "/tmp/boltstream-smoke-live.sh"
$LocalScript = Join-Path $env:TEMP "boltstream-smoke-live.sh"

$SmokeScript = @'
#!/usr/bin/env bash
set -euo pipefail
echo "live:"
curl -fsS http://127.0.0.1:9100/health/live
echo
echo "ready:"
curl -fsS http://127.0.0.1:9100/health/ready
echo
echo "version:"
curl -fsS http://127.0.0.1:9100/version
echo
echo "topics:"
find /var/lib/boltstream/topics -maxdepth 4 -type f -print 2>/dev/null || true
echo "journal:"
ACTIVE_SINCE="$(systemctl show -p ActiveEnterTimestamp --value boltstream.service)"
journalctl -u boltstream.service --since "${ACTIVE_SINCE}" -n 80 --no-pager
'@
[System.IO.File]::WriteAllText($LocalScript, $SmokeScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

& $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $LocalScript "${InstanceName}:$RemoteScript"
if ($LASTEXITCODE -ne 0) { throw "Failed to copy smoke script to $InstanceName." }
$Output = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $RemoteScript"
if ($LASTEXITCODE -ne 0) { throw "Remote smoke script failed on $InstanceName." }
$Output

$OutputText = $Output -join "`n"
if ($ExpectedGitSha -and ($OutputText -notmatch [regex]::Escape($ExpectedGitSha))) {
  throw "Expected git SHA '$ExpectedGitSha' was not found in live /version output."
}
