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
  throw "Refusing live smoke. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

$RemoteScript = "/tmp/boltstream-phase11-$ExpectedGitSha.sh"
$LocalScript = Join-Path $env:TEMP "boltstream-phase11-$ExpectedGitSha.sh"
$Script = @'
#!/usr/bin/env bash
set -euo pipefail
SHA="__SHA__"
ROOT="/var/lib/boltstream/phase11-live"
BROKER=19100
ADMIN=19101
SERVER="/opt/boltstream/releases/$SHA/bin/boltstream-server"
ADMIN_TOOL="/opt/boltstream/releases/$SHA/bin/boltstream-admin"
PRODUCER="/opt/boltstream/releases/$SHA/bin/boltstream-producer"
CONSUMER="/opt/boltstream/releases/$SHA/bin/boltstream-consumer"
LOGTOOL="/opt/boltstream/releases/$SHA/bin/boltstream-logtool"
PID=""
cleanup() {
  if [[ -n "$PID" ]]; then sudo kill "$PID" 2>/dev/null || true; fi
  sudo rm -rf "$ROOT" /tmp/boltstream-phase11.out /tmp/boltstream-phase11.err
}
trap cleanup EXIT

version="$(curl -fsS http://127.0.0.1:9100/version)"
[[ "$version" == *"$SHA"* ]] || { echo "normal runtime SHA mismatch" >&2; exit 1; }
set -a
source <(sudo cat /etc/boltstream/boltstream.env)
set +a
sudo rm -rf "$ROOT"
sudo install -d -o boltstream -g boltstream "$ROOT"
sudo -u boltstream env BOLTSTREAM_BROKER_TOKEN="$BOLTSTREAM_BROKER_TOKEN" \
  "$SERVER" --listen "127.0.0.1:$BROKER" --admin-listen "127.0.0.1:$ADMIN" \
  --data "$ROOT" --max-fetch-records 1024 \
  >/tmp/boltstream-phase11.out 2>/tmp/boltstream-phase11.err &
PID=$!
for _ in $(seq 1 80); do
  curl -fsS "http://127.0.0.1:$ADMIN/health/ready" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -fsS "http://127.0.0.1:$ADMIN/health/ready"

for topic in phase11-none phase11-zstd; do
  "$ADMIN_TOOL" topics create --host 127.0.0.1 --port "$BROKER" --token "$BOLTSTREAM_BROKER_TOKEN" \
    --topic "$topic" --partitions 1
done
payload="$(printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZ%.0s' $(seq 1 10))"
none="$($PRODUCER --host 127.0.0.1 --port "$BROKER" --token "$BOLTSTREAM_BROKER_TOKEN" \
  --topic phase11-none --message "$payload" --batch-records 32 --compression none)"
zstd="$($PRODUCER --host 127.0.0.1 --port "$BROKER" --token "$BOLTSTREAM_BROKER_TOKEN" \
  --topic phase11-zstd --message "$payload" --batch-records 32 --compression zstd --zstd-level 3)"
fetch="$($CONSUMER --host 127.0.0.1 --port "$BROKER" --token "$BOLTSTREAM_BROKER_TOKEN" \
  --topic phase11-zstd --partition 0 --from beginning --compression zstd)"
metrics="$(curl -fsS "http://127.0.0.1:$ADMIN/metrics")"
grep -F '"record_count":32' <<<"$none" >/dev/null
grep -F '"record_count":32' <<<"$zstd" >/dev/null
grep -F '"count":32' <<<"$fetch" >/dev/null
grep -F 'boltstream_compression_batches_total{codec="zstd"} 1' <<<"$metrics" >/dev/null
grep -F 'boltstream_compressed_fetch_passthrough_total 1' <<<"$metrics" >/dev/null
none_bytes="$(stat -c %s "$ROOT/topics/phase11-none/partition-000000/00000000000000000000.log")"
zstd_bytes="$(stat -c %s "$ROOT/topics/phase11-zstd/partition-000000/00000000000000000000.log")"
(( zstd_bytes < none_bytes )) || { echo "zstd log was not smaller" >&2; exit 1; }
sudo kill "$PID"
wait "$PID" 2>/dev/null || true
PID=""
inspect="$(sudo -u boltstream "$LOGTOOL" inspect-batch --data "$ROOT" --topic phase11-zstd --from 0)"
recover="$(sudo -u boltstream "$LOGTOOL" recover --data "$ROOT" --topic phase11-zstd)"
grep -F '"codec":"zstd"' <<<"$inspect" >/dev/null
grep -F '"records_recovered":32' <<<"$recover" >/dev/null
echo "normal_version=$version"
echo "none_produce=$none"
echo "zstd_produce=$zstd"
echo "zstd_fetch_count=32"
echo "none_log_bytes=$none_bytes"
echo "zstd_log_bytes=$zstd_bytes"
echo "inspect=$inspect"
echo "recover=$recover"
echo "passthrough_metric=1"
'@
$Script = $Script.Replace("__SHA__", $ExpectedGitSha)
[System.IO.File]::WriteAllText($LocalScript, $Script.Replace("`r`n", "`n"),
  [System.Text.Encoding]::ASCII)
try {
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone `
    $LocalScript "${InstanceName}:$RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy Phase 11 live smoke." }
  & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId `
    --zone $Zone --command "bash $RemoteScript"
  if ($LASTEXITCODE -ne 0) { throw "Phase 11 live smoke failed." }
} finally {
  Remove-Item -LiteralPath $LocalScript -Force -ErrorAction SilentlyContinue
  & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId `
    --zone $Zone --command "rm -f $RemoteScript" 2>$null | Out-Null
}
