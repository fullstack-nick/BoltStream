param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [string]$GitSha = "",
  [string]$OutputDir = "artifacts/benchmarks/gcp",
  [int]$StartRound = 1,
  [int]$Rounds = 5,
  [switch]$Quick
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing live benchmark. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}
if ([string]::IsNullOrWhiteSpace($GitSha)) {
  $GitSha = (git rev-parse --short=12 HEAD).Trim()
}
if ($GitSha -notmatch '^[0-9a-f]{12}$') {
  throw "GitSha must be the deployed 12-character hexadecimal commit id."
}
if ($StartRound -lt 1) { throw "StartRound must be positive." }
if ($Rounds -lt 1) { throw "Rounds must be positive." }
if ($Quick) { $StartRound = 1; $Rounds = 1 }

$Profiles = [ordered]@{
  "single-threaded" = @{ Config = "benchmarks/profiles/single-threaded.yaml"; Io = 1; Append = 0; Batch = 1 }
  "worker-event-loops" = @{ Config = "benchmarks/profiles/worker-event-loops.yaml"; Io = 2; Append = 2; Batch = 1 }
  "batched-writes" = @{ Config = "benchmarks/profiles/batched-writes.yaml"; Io = 2; Append = 2; Batch = 32 }
}
$Order = @($Profiles.Keys)
$Duration = if ($Quick) { 1 } else { 30 }
$WarmupSeconds = if ($Quick) { 0 } else { 60 }
$Messages = if ($Quick) { 400 } else { 100000 }
$FetchMessages = if ($Quick) { 400 } else { 250000 }
$WarmupMessages = if ($Quick) { 20 } else { 10000 }
$Clients = if ($Quick) { 8 } else { 16 }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$Version = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "curl -fsS http://127.0.0.1:9100/version"
if ($LASTEXITCODE -ne 0 -or ($Version -join "`n") -notmatch [regex]::Escape($GitSha)) {
  throw "Live broker /version does not match requested SHA $GitSha."
}

foreach ($Profile in $Profiles.GetEnumerator()) {
  $RemoteConfig = "/tmp/phase10-$($Profile.Key).yaml"
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $Profile.Value.Config "${InstanceName}:$RemoteConfig"
  if ($LASTEXITCODE -ne 0) { throw "Failed to upload profile $($Profile.Key)." }
}

try {
  $EndRound = $StartRound + $Rounds - 1
  for ($Round = $StartRound; $Round -le $EndRound; $Round++) {
    $Shift = ($Round - 1) % $Order.Count
    if ($Shift -eq 0) {
      $RoundOrder = $Order
    } else {
      $RoundOrder = @($Order[$Shift..($Order.Count - 1)] + $Order[0..($Shift - 1)])
    }
    foreach ($ProfileName in $RoundOrder) {
      $Metadata = $Profiles[$ProfileName]
      $RemoteScript = "/tmp/phase10-$ProfileName-r$Round.sh"
      $LocalScript = Join-Path $env:TEMP "phase10-$ProfileName-r$Round.sh"
      $RemotePrefix = "/tmp/phase10-$ProfileName-r$Round"
      $Script = @"
#!/usr/bin/env bash
set -euo pipefail

PROFILE='$ProfileName'
ROUND='$Round'
SHA='$GitSha'
CONFIG='/tmp/phase10-$ProfileName.yaml'
DATA='/var/lib/boltstream/phase10-$ProfileName-r$Round'
DROPIN='/etc/systemd/system/boltstream.service.d/phase10-benchmark.conf'

cleanup() {
  sudo rm -f "`$DROPIN"
  sudo systemctl daemon-reload
  sudo systemctl restart boltstream.service
  for attempt in {1..60}; do
    curl -fsS http://127.0.0.1:9100/health/ready >/dev/null 2>&1 && break
    sleep 1
  done
  sudo rm -rf "`$DATA"
}
trap cleanup EXIT

available=`$(df --output=avail -B1 /var/lib/boltstream | tail -1 | tr -d ' ')
if [ "`$available" -lt 10737418240 ]; then
  echo "phase10 benchmark requires at least 10 GiB available" >&2
  exit 1
fi

sudo rm -rf "`$DATA"
sudo install -d -o boltstream -g boltstream "`$DATA"
effective=`$(sudo -u boltstream /opt/boltstream/current/bin/boltstream-server --config "`$CONFIG" --data "`$DATA" --print-effective-config)
grep -F 'io_workers: $($Metadata.Io)' <<<"`$effective" >/dev/null
grep -F 'append_workers: $($Metadata.Append)' <<<"`$effective" >/dev/null
grep -F 'append_batch_records: $($Metadata.Batch)' <<<"`$effective" >/dev/null
sudo install -d /etc/systemd/system/boltstream.service.d
sudo tee "`$DROPIN" >/dev/null <<EOF
[Service]
ExecStart=
ExecStart=/opt/boltstream/current/bin/boltstream-server --config `$CONFIG --data `$DATA
EOF
sudo systemctl daemon-reload
sudo systemctl restart boltstream.service
sudo systemctl is-active --quiet boltstream.service
for attempt in {1..60}; do
  curl -fsS http://127.0.0.1:9100/health/ready >/dev/null 2>&1 && break
  sleep 1
done
version=`$(curl -fsS http://127.0.0.1:9100/version)
case "`$version" in *"`$SHA"*) ;; *) echo "profile runtime SHA mismatch: `$version" >&2; exit 1;; esac

run_bench() {
  workload="`$1"
  messages="`$2"
  output="$RemotePrefix-`$workload.json"
  sudo -u boltstream bash -c "set -a; source /etc/boltstream/boltstream.env; exec /opt/boltstream/current/bin/boltstream-bench run \\
    --workload '`$workload' --host 127.0.0.1 --port 9000 --admin-port 9100 \\
    --profile '$ProfileName' --environment gcp-e2-micro --machine-type e2-micro \\
    --partitions 4 --clients '$Clients' --duration-seconds '$Duration' \\
    --warmup-seconds '$WarmupSeconds' --messages '`$messages' \\
    --warmup-messages '$WarmupMessages' --payload-bytes 256 --key-bytes 16 --repetitions 1 \\
    --server-io-workers '$($Metadata.Io)' --server-append-workers '$($Metadata.Append)' \\
    --server-append-batch-records '$($Metadata.Batch)' --server-queue-depth 1024 \\
    --server-log-level warn --json-out '`$output'" >/dev/null
}

run_bench produce-throughput '$Messages'
run_bench produce-latency '$Messages'
run_bench fetch-throughput '$FetchMessages'
curl -fsS http://127.0.0.1:9100/health/ready >/dev/null
"@
      [System.IO.File]::WriteAllText($LocalScript, $Script.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)
      try {
        & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $LocalScript "${InstanceName}:$RemoteScript"
        if ($LASTEXITCODE -ne 0) { throw "Failed to upload live benchmark runner." }
        & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $RemoteScript"
        if ($LASTEXITCODE -ne 0) { throw "Live benchmark failed for $ProfileName round $Round." }
        foreach ($Workload in @("produce-throughput", "produce-latency", "fetch-throughput")) {
          $LocalResult = Join-Path $OutputDir "$ProfileName-$Workload-r$Round.json"
          & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone "${InstanceName}:$RemotePrefix-$Workload.json" $LocalResult
          if ($LASTEXITCODE -ne 0) { throw "Failed to download $ProfileName/$Workload round $Round." }
          $Result = Get-Content $LocalResult -Raw | ConvertFrom-Json
          if ($Result.summary.errors -ne 0) { throw "$ProfileName/$Workload round $Round reported errors." }
          if ($Workload -eq "produce-throughput") {
            $Batches = [double](($Result.repetitions | Measure-Object -Property append_batches -Sum).Sum)
            $Records = [double](($Result.repetitions | Measure-Object -Property append_batch_records -Sum).Sum)
            if ($ProfileName -eq "batched-writes" -and
                ($Batches -le 0 -or $Records / $Batches -le 1.0)) {
              throw "Batched GCP profile did not prove an average batch size above one."
            }
            if ($ProfileName -ne "batched-writes" -and $Batches -ne $Records) {
              throw "Unbatched GCP profile $ProfileName emitted a batch larger than one."
            }
          }
        }
        & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "test ! -e /var/lib/boltstream/phase10-$ProfileName-r$Round; test ! -e /etc/systemd/system/boltstream.service.d/phase10-benchmark.conf; systemctl is-active --quiet boltstream.service"
        if ($LASTEXITCODE -ne 0) { throw "Profile $ProfileName round $Round did not clean up completely." }
        Write-Host "GCP Phase 10 round $Round passed: $ProfileName"
      } finally {
        Remove-Item -Force -LiteralPath $LocalScript -ErrorAction SilentlyContinue
        & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "rm -f $RemoteScript $RemotePrefix-*.json" | Out-Null
      }
    }
  }
} finally {
  & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "sudo rm -f /etc/systemd/system/boltstream.service.d/phase10-benchmark.conf; sudo systemctl daemon-reload; sudo systemctl restart boltstream.service; rm -f /tmp/phase10-*.yaml /tmp/phase10-*.sh /tmp/phase10-*.json; sudo rm -rf /var/lib/boltstream/phase10-*" | Out-Null
}

$Final = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "systemctl is-active boltstream.service; curl -fsS http://127.0.0.1:9100/health/ready; curl -fsS http://127.0.0.1:9100/version"
if ($LASTEXITCODE -ne 0 -or ($Final -join "`n") -notmatch [regex]::Escape($GitSha)) {
  throw "Normal BoltStream service was not restored after the live benchmark."
}
Write-Host "GCP Phase 10 benchmark matrix passed and normal service was restored. Results: $OutputDir"
