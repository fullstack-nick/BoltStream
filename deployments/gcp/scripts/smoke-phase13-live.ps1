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
  throw "Refusing live Phase 13 smoke. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$RemoteScript = "/tmp/boltstream-phase13-live-$ExpectedGitSha.sh"
$LocalScript = Join-Path $env:TEMP "boltstream-phase13-live-$ExpectedGitSha.sh"
$Root = "/var/lib/boltstream/phase13-live"
$Script = @"
#!/usr/bin/env bash
set -euo pipefail

EXPECTED_SHA="$ExpectedGitSha"
ROOT="$Root"
BINARY="/opt/boltstream/current/bin/boltstream-recovery-proof"

test -x "`$BINARY"
VERSION="`$(curl -fsS http://127.0.0.1:9100/version)"
echo "version: `$VERSION"
grep -F "\"git_sha\":\"`$EXPECTED_SHA\"" <<<"`$VERSION" >/dev/null
grep -F '"protocol_version":5' <<<"`$VERSION" >/dev/null
grep -F '"storage_format_version":3' <<<"`$VERSION" >/dev/null

sudo rm -rf "`$ROOT"
sudo install -d -o boltstream -g boltstream -m 0750 "`$ROOT"
OUTPUT="`$(sudo -u boltstream "`$BINARY" --root "`$ROOT" --keep-data)"
echo "`$OUTPUT"
grep -F '"scenario":"torn-record","worker_crashed":true' <<<"`$OUTPUT" >/dev/null
grep -F '"scenario":"partial-batch","worker_crashed":true' <<<"`$OUTPUT" >/dev/null
grep -F '"scenario":"stale-index","worker_crashed":true' <<<"`$OUTPUT" >/dev/null
test "`$(grep -Fc '"records_recovered":3,"next_offset":3' <<<"`$OUTPUT")" = 3
test "`$(grep -Fc '"indexes_rebuilt":1' <<<"`$OUTPUT")" = 3
grep -F '"status":"ok"' <<<"`$OUTPUT" >/dev/null
grep -F '"records_exact":true' <<<"`$OUTPUT" >/dev/null

echo 'recovered files:'
find "`$ROOT" -type f -printf '%P %s bytes\n' | sort
test "`$(find "`$ROOT" -name '*.log' -type f | wc -l)" = 3
test "`$(find "`$ROOT" -name '*.index' -type f | wc -l)" = 3
test "`$(find "`$ROOT" -name '*.index' -type f -size 60c | wc -l)" = 3

sudo rm -rf "`$ROOT"
test ! -e "`$ROOT"
test "`$(systemctl is-active boltstream.service)" = active
curl -fsS http://127.0.0.1:9100/health/ready
echo
curl -fsS http://127.0.0.1:9100/version
echo
echo 'phase13 cleanup: clean'
"@
[System.IO.File]::WriteAllText(
  $LocalScript,
  $Script.Replace("`r`n", "`n"),
  [System.Text.Encoding]::ASCII)

try {
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone `
    $LocalScript "${InstanceName}:$RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy Phase 13 live smoke to $InstanceName." }

  $Output = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no `
    --project $ProjectId --zone $Zone --command "bash $RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Phase 13 live smoke failed on $InstanceName." }
  $Output
} finally {
  & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no `
    --project $ProjectId --zone $Zone `
    --command "sudo rm -rf '$Root'; rm -f '$RemoteScript'" 2>$null | Out-Null
  Remove-Item -Force -LiteralPath $LocalScript -ErrorAction SilentlyContinue
}
