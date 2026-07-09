param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [int]$LocalPort = 19100
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing metrics tunnel. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}
$ActiveProject = (& $Gcloud config get-value project 2>$null).Trim()
if ($ActiveProject -ne $ProjectId) {
  throw "Refusing metrics tunnel. Active project is '$ActiveProject', expected '$ProjectId'."
}
if ($LocalPort -lt 1 -or $LocalPort -gt 65535) {
  throw "LocalPort must be between 1 and 65535."
}

Write-Host "Forwarding http://127.0.0.1:$LocalPort to $InstanceName 127.0.0.1:9100."
Write-Host "Keep this process running while Prometheus or curl inspects the live broker."
& $Gcloud compute ssh $InstanceName `
  --strict-host-key-checking=no `
  --project $ProjectId `
  --zone $Zone `
  --ssh-flag=-N `
  --ssh-flag=-L `
  --ssh-flag="127.0.0.1:${LocalPort}:127.0.0.1:9100"
if ($LASTEXITCODE -ne 0) {
  throw "Metrics SSH tunnel exited with code $LASTEXITCODE."
}
