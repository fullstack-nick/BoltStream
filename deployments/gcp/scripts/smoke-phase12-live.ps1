param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [Parameter(Mandatory = $true)][string]$ExpectedGitSha
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing live Phase 12 smoke. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$RemoteScript = "/tmp/boltstream-phase12-live-$ExpectedGitSha.sh"
$LocalScript = Join-Path $env:TEMP "boltstream-phase12-live-$ExpectedGitSha.sh"
$Root = "/var/lib/boltstream/phase12-live"
$Script = @"
#!/usr/bin/env bash
set -euo pipefail

EXPECTED_SHA="$ExpectedGitSha"
ROOT="$Root"
BINARY="/opt/boltstream/current/bin/boltstream-replication-sim"

test -x "`$BINARY"
VERSION="`$(curl -fsS http://127.0.0.1:9100/version)"
echo "version: `$VERSION"
grep -F "\"git_sha\":\"`$EXPECTED_SHA\"" <<<"`$VERSION" >/dev/null

sudo rm -rf "`$ROOT"
sudo install -d -o boltstream -g boltstream -m 0750 "`$ROOT"
OUTPUT="`$(sudo -u boltstream "`$BINARY" --root "`$ROOT" --compression zstd --keep-data)"
echo "`$OUTPUT"
grep -F '"status":"ok"' <<<"`$OUTPUT" >/dev/null
grep -F '"leader_ack_while_offline":true' <<<"`$OUTPUT" >/dev/null
grep -F '"all_timeout_observed":true' <<<"`$OUTPUT" >/dev/null
grep -F '"restart_offset_before":4' <<<"`$OUTPUT" >/dev/null
grep -F '"restart_offset_after":4' <<<"`$OUTPUT" >/dev/null
grep -F '"records_exact":true' <<<"`$OUTPUT" >/dev/null
grep -F '"lag_records":0' <<<"`$OUTPUT" >/dev/null
grep -F 'boltstream_replication_lag_records' <<<"`$OUTPUT" >/dev/null

echo 'replica files:'
find "`$ROOT" -type f -printf '%P %s bytes\n' | sort
LEADER_FILES="`$(find "`$ROOT/leader" -type f | wc -l)"
FOLLOWER_FILES="`$(find "`$ROOT/follower" -type f | wc -l)"
test "`$LEADER_FILES" -gt 0
test "`$FOLLOWER_FILES" -gt 0

sudo rm -rf "`$ROOT"
test ! -e "`$ROOT"
test "`$(systemctl is-active boltstream.service)" = active
curl -fsS http://127.0.0.1:9100/health/ready
echo
curl -fsS http://127.0.0.1:9100/version
echo
echo 'phase12 cleanup: clean'
"@
[System.IO.File]::WriteAllText(
  $LocalScript,
  $Script.Replace("`r`n", "`n"),
  [System.Text.Encoding]::ASCII)

try {
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone `
    $LocalScript "${InstanceName}:$RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy Phase 12 live smoke to $InstanceName." }

  $Output = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no `
    --project $ProjectId --zone $Zone --command "bash $RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Phase 12 live smoke failed on $InstanceName." }
  $Output
} finally {
  & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no `
    --project $ProjectId --zone $Zone `
    --command "sudo rm -rf '$Root'; rm -f '$RemoteScript'" 2>$null | Out-Null
  Remove-Item -Force -LiteralPath $LocalScript -ErrorAction SilentlyContinue
}
