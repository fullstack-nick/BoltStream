param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [string]$ExpectedGitSha = ""
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
