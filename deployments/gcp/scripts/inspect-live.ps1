param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$ActiveAccount = (gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing live inspection. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$RemoteCommand = @'
set -euo pipefail
echo "== systemd =="
systemctl --no-pager --full status boltstream.service
echo "== journal =="
journalctl -u boltstream.service -n 80 --no-pager
echo "== version =="
curl -fsS http://127.0.0.1:9100/version
echo
echo "== data dir =="
df -h /var/lib/boltstream
ls -la /var/lib/boltstream
echo "== release =="
readlink -f /opt/boltstream/current
ls -la /opt/boltstream/current/bin
'@

gcloud compute ssh $InstanceName --project $ProjectId --zone $Zone --command $RemoteCommand

