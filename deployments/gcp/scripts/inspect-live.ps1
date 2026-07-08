param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing live inspection. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$RemoteScript = "/tmp/boltstream-inspect-live.sh"
$LocalScript = Join-Path $env:TEMP "boltstream-inspect-live.sh"

$InspectScript = @'
#!/usr/bin/env bash
set -euo pipefail
echo "== systemd =="
systemctl --no-pager --full status boltstream.service
echo "== journal =="
ACTIVE_SINCE="$(systemctl show -p ActiveEnterTimestamp --value boltstream.service)"
journalctl -u boltstream.service --since "${ACTIVE_SINCE}" -n 80 --no-pager
echo "== version =="
curl -fsS http://127.0.0.1:9100/version
echo
echo "== data dir =="
df -h /var/lib/boltstream
ls -la /var/lib/boltstream
sudo -u boltstream test -w /var/lib/boltstream
echo "data dir writable by boltstream"
echo "== topic files =="
find /var/lib/boltstream/topics -maxdepth 4 -type f -print 2>/dev/null || true
echo "== release =="
readlink -f /opt/boltstream/current
ls -la /opt/boltstream/current/bin
'@
[System.IO.File]::WriteAllText($LocalScript, $InspectScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

& $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $LocalScript "${InstanceName}:$RemoteScript"
if ($LASTEXITCODE -ne 0) { throw "Failed to copy inspection script to $InstanceName." }
& $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $RemoteScript"
if ($LASTEXITCODE -ne 0) { throw "Remote inspection script failed on $InstanceName." }
