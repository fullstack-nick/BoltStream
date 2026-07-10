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
  throw "Refusing live inspection. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}
if ($ExpectedGitSha -and $ExpectedGitSha -notmatch '^[0-9a-f]{7,40}$') {
  throw "ExpectedGitSha must contain 7 to 40 lowercase hexadecimal characters."
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
VERSION="$(curl -fsS http://127.0.0.1:9100/version)"
echo "$VERSION"
if [[ -n '__EXPECTED_GIT_SHA__' ]] && [[ "$VERSION" != *'"git_sha":"__EXPECTED_GIT_SHA__"'* ]]; then
  echo "expected Git SHA __EXPECTED_GIT_SHA__ was not live" >&2
  exit 1
fi
echo
echo "== metrics summary =="
curl -fsS http://127.0.0.1:9100/metrics | grep -E '^boltstream_(build_info|ready|uptime_seconds|storage_(capacity|available)_bytes|metrics_scrapes_total)' || true
echo "== config =="
stat -c '%U %G %a %n' /etc/boltstream/boltstream.yaml /etc/boltstream/boltstream.env
sudo -u boltstream /opt/boltstream/current/bin/boltstream-server --config /etc/boltstream/boltstream.yaml --check-config
sudo -u boltstream cat /etc/boltstream/boltstream.yaml
echo "== data dir =="
df -h /var/lib/boltstream
ls -la /var/lib/boltstream
sudo -u boltstream test -w /var/lib/boltstream
echo "data dir writable by boltstream"
echo "== topic files =="
find /var/lib/boltstream/topics -maxdepth 4 -type f -print 2>/dev/null || true
echo "== consumer offset files =="
find /var/lib/boltstream/consumer_offsets -maxdepth 3 -type f -print -exec tail -n 5 {} \; 2>/dev/null || true
echo "== release =="
readlink -f /opt/boltstream/current
ls -la /opt/boltstream/current/bin
'@
$InspectScript = $InspectScript.Replace('__EXPECTED_GIT_SHA__', $ExpectedGitSha)
[System.IO.File]::WriteAllText($LocalScript, $InspectScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

& $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $LocalScript "${InstanceName}:$RemoteScript"
if ($LASTEXITCODE -ne 0) { throw "Failed to copy inspection script to $InstanceName." }
& $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $RemoteScript"
if ($LASTEXITCODE -ne 0) { throw "Remote inspection script failed on $InstanceName." }
