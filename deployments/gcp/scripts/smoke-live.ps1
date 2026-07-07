param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [string]$ExpectedGitSha = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$ActiveAccount = (gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing live smoke. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$ExternalIp = (gcloud compute instances describe $InstanceName --project $ProjectId --zone $Zone --format="value(networkInterfaces[0].accessConfigs[0].natIP)").Trim()
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

$RemoteCommand = @'
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

$Output = gcloud compute ssh $InstanceName --project $ProjectId --zone $Zone --command $RemoteCommand
$Output

if ($ExpectedGitSha -and ($Output -notmatch [regex]::Escape($ExpectedGitSha))) {
  throw "Expected git SHA '$ExpectedGitSha' was not found in live /version output."
}

