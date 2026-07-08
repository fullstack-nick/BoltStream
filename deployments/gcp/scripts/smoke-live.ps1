param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [string]$ExpectedGitSha = "",
  [string]$BuildDir = ""
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

$BrokerTokenOutput = & $Gcloud secrets versions access latest --secret "boltstream-broker-token" --project $ProjectId 2>$null
$BrokerToken = ($BrokerTokenOutput -join "`n").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($BrokerToken)) {
  throw "Secret Manager secret 'boltstream-broker-token' must have a non-empty latest version for live smoke."
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot "build\windows-gcc-debug"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $RepoRoot $BuildDir
}

function Get-ToolPath {
  param(
    [Parameter(Mandatory = $true)][string]$Name
  )

  $exe = Join-Path $BuildDir "$Name.exe"
  if (Test-Path $exe) { return $exe }
  $plain = Join-Path $BuildDir $Name
  if (Test-Path $plain) { return $plain }
  throw "Required binary not found: $exe"
}

$Producer = Get-ToolPath -Name "boltstream-producer"
$Consumer = Get-ToolPath -Name "boltstream-consumer"
$Admin = Get-ToolPath -Name "boltstream-admin"

function Stop-ProcessIfRunning {
  param([object]$Process)

  if ($Process -and -not $Process.HasExited) {
    Stop-Process -Id $Process.Id -Force
    $Process.WaitForExit()
  }
}

function Wait-FileContains {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Text,
    [int]$TimeoutMs = 15000
  )

  $deadline = [DateTimeOffset]::UtcNow.AddMilliseconds($TimeoutMs)
  do {
    $content = ""
    if (Test-Path $Path) {
      $raw = Get-Content -Raw $Path
      if ($null -ne $raw) { $content = [string]$raw }
    }
    if ($content.Contains($Text)) { return }
    Start-Sleep -Milliseconds 150
  } while ([DateTimeOffset]::UtcNow -lt $deadline)

  $content = ""
  if (Test-Path $Path) {
    $raw = Get-Content -Raw $Path
    if ($null -ne $raw) { $content = [string]$raw }
  }
  throw "Timed out waiting for '$Text' in '$Path'. Content: $content"
}

function Wait-FilePatternCount {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Text,
    [Parameter(Mandatory = $true)][int]$MinimumCount,
    [int]$TimeoutMs = 15000
  )

  $deadline = [DateTimeOffset]::UtcNow.AddMilliseconds($TimeoutMs)
  do {
    $content = ""
    if (Test-Path $Path) {
      $raw = Get-Content -Raw $Path
      if ($null -ne $raw) { $content = [string]$raw }
    }
    $count = [regex]::Matches($content, [regex]::Escape($Text)).Count
    if ($count -ge $MinimumCount) { return }
    Start-Sleep -Milliseconds 150
  } while ([DateTimeOffset]::UtcNow -lt $deadline)

  $content = ""
  if (Test-Path $Path) {
    $raw = Get-Content -Raw $Path
    if ($null -ne $raw) { $content = [string]$raw }
  }
  throw "Timed out waiting for '$Text' count $MinimumCount in '$Path'. Content: $content"
}

function Get-FilePatternCount {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Text
  )

  $content = ""
  if (Test-Path $Path) {
    $raw = Get-Content -Raw $Path
    if ($null -ne $raw) { $content = [string]$raw }
  }
  return [regex]::Matches($content, [regex]::Escape($Text)).Count
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

$Topic = "live-phase7-basic-$([DateTimeOffset]::UtcNow.ToString('yyyyMMddHHmmss'))"
$CreateOutput = & $Admin topics create --host $ExternalIp --port 9000 --token $BrokerToken --topic $Topic --partitions 3
if ($LASTEXITCODE -ne 0) {
  throw "Live create-topic failed with exit code $LASTEXITCODE. Output: $($CreateOutput -join "`n")"
}
$Created = ($CreateOutput -join "`n") | ConvertFrom-Json
if (($Created.status -ne "created" -and $Created.status -ne "exists") -or $Created.partitions -ne 3) {
  throw "Unexpected live create-topic output: $($Created | ConvertTo-Json -Compress)"
}

$ProduceOutput = & $Producer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Topic --key AAPL --message "AAPL,100,192.41"
if ($LASTEXITCODE -ne 0) {
  throw "Live producer failed with exit code $LASTEXITCODE. Output: $($ProduceOutput -join "`n")"
}
$Produced = ($ProduceOutput -join "`n") | ConvertFrom-Json
if ($Produced.status -ne "ok" -or $Produced.offset -ne 0 -or $Produced.next_offset -ne 1) {
  throw "Unexpected live producer output: $($Produced | ConvertTo-Json -Compress)"
}

$ConsumerOutput = & $Consumer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Topic --partition $Produced.partition --from beginning
if ($LASTEXITCODE -ne 0) {
  throw "Live consumer failed with exit code $LASTEXITCODE. Output: $($ConsumerOutput -join "`n")"
}
$Consumed = ($ConsumerOutput -join "`n") | ConvertFrom-Json
if ($Consumed.status -ne "ok" -or $Consumed.count -ne 1 -or
    $Consumed.records[0].key -ne "AAPL" -or
    $Consumed.records[0].message -ne "AAPL,100,192.41") {
  throw "Unexpected live consumer output: $($Consumed | ConvertTo-Json -Compress)"
}
Write-Host "Live broker produce/fetch succeeded for topic $Topic."

$CommitOutput = & $Consumer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Topic --partition $Produced.partition --group livephase7basic --from beginning --commit
if ($LASTEXITCODE -ne 0) {
  throw "Live group commit failed with exit code $LASTEXITCODE. Output: $($CommitOutput -join "`n")"
}
$Committed = ($CommitOutput -join "`n") | ConvertFrom-Json
if ($Committed.status -ne "ok" -or $Committed.count -ne 1 -or $Committed.committed_offset -ne 1) {
  throw "Unexpected live group commit output: $($Committed | ConvertTo-Json -Compress)"
}

$ResumeOutput = & $Consumer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Topic --partition $Produced.partition --group livephase7basic
if ($LASTEXITCODE -ne 0) {
  throw "Live committed resume failed with exit code $LASTEXITCODE. Output: $($ResumeOutput -join "`n")"
}
$Resumed = ($ResumeOutput -join "`n") | ConvertFrom-Json
if ($Resumed.status -ne "ok" -or $Resumed.from -ne 1 -or $Resumed.count -ne 0) {
  throw "Unexpected live committed resume output: $($Resumed | ConvertTo-Json -Compress)"
}
Write-Host "Live broker group commit/resume succeeded for topic $Topic."

$CoordinatedTopic = "live-phase7-$([DateTimeOffset]::UtcNow.ToString('yyyyMMddHHmmss'))"
$CreateOutput = & $Admin topics create --host $ExternalIp --port 9000 --token $BrokerToken --topic $CoordinatedTopic --partitions 4
if ($LASTEXITCODE -ne 0) {
  throw "Live coordinated create-topic failed with exit code $LASTEXITCODE. Output: $($CreateOutput -join "`n")"
}
$Created = ($CreateOutput -join "`n") | ConvertFrom-Json
if (($Created.status -ne "created" -and $Created.status -ne "exists") -or $Created.partitions -ne 4) {
  throw "Unexpected live coordinated create-topic output: $($Created | ConvertTo-Json -Compress)"
}

$Consumer1Out = Join-Path $env:TEMP "boltstream-live-phase7-consumer1.out"
$Consumer1Err = Join-Path $env:TEMP "boltstream-live-phase7-consumer1.err"
$Consumer2Out = Join-Path $env:TEMP "boltstream-live-phase7-consumer2.out"
$Consumer2Err = Join-Path $env:TEMP "boltstream-live-phase7-consumer2.err"
Remove-Item -Force -LiteralPath $Consumer1Out, $Consumer1Err, $Consumer2Out, $Consumer2Err -ErrorAction SilentlyContinue
$consumer1 = $null
$consumer2 = $null
try {
  $baseConsumerArgs = @(
    "--host", $ExternalIp, "--port", "9000", "--token", $BrokerToken,
    "--topic", $CoordinatedTopic, "--group", "livephase7", "--commit", "--coordinated",
    "--session-timeout-ms", "5000", "--heartbeat-ms", "1000", "--poll-ms", "100",
    "--idle-exit-ms", "20000", "--timeout-ms", "7000"
  )
  $consumer1 = Start-Process `
    -FilePath $Consumer `
    -ArgumentList $baseConsumerArgs `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $Consumer1Out `
    -RedirectStandardError $Consumer1Err
  Start-Sleep -Milliseconds 300
  $consumer2 = Start-Process `
    -FilePath $Consumer `
    -ArgumentList $baseConsumerArgs `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $Consumer2Out `
    -RedirectStandardError $Consumer2Err

  Wait-FileContains $Consumer1Out '"partitions":[0,1]'
  Wait-FileContains $Consumer2Out '"partitions":[2,3]'

  for ($i = 0; $i -lt 4; ++$i) {
    $ProduceOutput = & $Producer --host $ExternalIp --port 9000 --token $BrokerToken --topic $CoordinatedTopic --message "live-share-$i"
    if ($LASTEXITCODE -ne 0) {
      throw "Live coordinated produce failed with exit code $LASTEXITCODE. Output: $($ProduceOutput -join "`n")"
    }
    $Produced = ($ProduceOutput -join "`n") | ConvertFrom-Json
    if ($Produced.partition -ne $i) {
      throw "Expected live-share-$i on partition $i, got $($Produced | ConvertTo-Json -Compress)"
    }
  }

  Wait-FileContains $Consumer1Out '"message":"live-share-0"'
  Wait-FileContains $Consumer1Out '"message":"live-share-1"'
  Wait-FileContains $Consumer2Out '"message":"live-share-2"'
  Wait-FileContains $Consumer2Out '"message":"live-share-3"'

  $fullAssignmentBeforeTakeover = Get-FilePatternCount $Consumer1Out '"partitions":[0,1,2,3]'
  Stop-ProcessIfRunning -Process $consumer2
  $consumer2 = $null
  Wait-FilePatternCount $Consumer1Out '"partitions":[0,1,2,3]' ($fullAssignmentBeforeTakeover + 1) 22000

  for ($i = 0; $i -lt 4; ++$i) {
    $ProduceOutput = & $Producer --host $ExternalIp --port 9000 --token $BrokerToken --topic $CoordinatedTopic --message "live-takeover-$i"
    if ($LASTEXITCODE -ne 0) {
      throw "Live coordinated takeover produce failed with exit code $LASTEXITCODE. Output: $($ProduceOutput -join "`n")"
    }
    $Produced = ($ProduceOutput -join "`n") | ConvertFrom-Json
    if ($Produced.partition -ne $i) {
      throw "Expected live-takeover-$i on partition $i, got $($Produced | ConvertTo-Json -Compress)"
    }
  }

  for ($i = 0; $i -lt 4; ++$i) {
    Wait-FileContains $Consumer1Out "`"message`":`"live-takeover-$i`""
  }

  if (-not $consumer1.WaitForExit(32000)) {
    throw "Live coordinated survivor did not exit after idle timeout"
  }
  $consumer1.Refresh()
  if ($null -ne $consumer1.ExitCode -and $consumer1.ExitCode -ne 0) {
    $err = if (Test-Path $Consumer1Err) { Get-Content -Raw $Consumer1Err } else { "" }
    throw "Live coordinated survivor exited with $($consumer1.ExitCode): $err"
  }
  Wait-FileContains $Consumer1Out '"event":"summary"' 1000
  $consumer1 = $null
} finally {
  Stop-ProcessIfRunning -Process $consumer2
  Stop-ProcessIfRunning -Process $consumer1
}
Write-Host "Live coordinated group split and timeout takeover succeeded for topic $CoordinatedTopic."

$Phase8OverrideScript = @"
#!/usr/bin/env bash
set -euo pipefail
sudo mkdir -p /etc/systemd/system/boltstream.service.d
sudo tee /etc/systemd/system/boltstream.service.d/phase8-smoke.conf >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream --segment-bytes 96 --segment-max-age-seconds 0 --retention-check-interval-ms 0
EOF
sudo systemctl daemon-reload
sudo systemctl restart boltstream.service
for i in {1..50}; do
  if curl -fsS http://127.0.0.1:9100/health/ready >/dev/null; then
    exit 0
  fi
  sleep 0.2
done
curl -fsS http://127.0.0.1:9100/health/ready >/dev/null
"@
$Phase8OverrideLocal = Join-Path $env:TEMP "boltstream-phase8-override.sh"
$Phase8OverrideRemote = "/tmp/boltstream-phase8-override.sh"
[System.IO.File]::WriteAllText($Phase8OverrideLocal, $Phase8OverrideScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

$Phase8CleanupScript = @'
#!/usr/bin/env bash
set -euo pipefail
sudo rm -f /etc/systemd/system/boltstream.service.d/phase8-smoke.conf
sudo rmdir /etc/systemd/system/boltstream.service.d 2>/dev/null || true
sudo systemctl daemon-reload
sudo systemctl restart boltstream.service
for i in {1..50}; do
  if curl -fsS http://127.0.0.1:9100/health/ready >/dev/null; then
    exit 0
  fi
  sleep 0.2
done
curl -fsS http://127.0.0.1:9100/health/ready >/dev/null
'@
$Phase8CleanupLocal = Join-Path $env:TEMP "boltstream-phase8-cleanup.sh"
$Phase8CleanupRemote = "/tmp/boltstream-phase8-cleanup.sh"
[System.IO.File]::WriteAllText($Phase8CleanupLocal, $Phase8CleanupScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

try {
  Write-Host "Applying temporary Phase 8 live retention settings."
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $Phase8OverrideLocal "${InstanceName}:$Phase8OverrideRemote"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy Phase 8 override script to $InstanceName." }
  & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $Phase8OverrideRemote"
  if ($LASTEXITCODE -ne 0) { throw "Failed to apply Phase 8 override on $InstanceName." }

  $Phase8Topic = "live-phase8-$([DateTimeOffset]::UtcNow.ToString('yyyyMMddHHmmss'))"
  $CreateOutput = & $Admin topics create --host $ExternalIp --port 9000 --token $BrokerToken --topic $Phase8Topic --partitions 1
  if ($LASTEXITCODE -ne 0) {
    throw "Live Phase 8 create-topic failed with exit code $LASTEXITCODE. Output: $($CreateOutput -join "`n")"
  }
  $Created = ($CreateOutput -join "`n") | ConvertFrom-Json
  if (($Created.status -ne "created" -and $Created.status -ne "exists") -or $Created.partitions -ne 1) {
    throw "Unexpected live Phase 8 create-topic output: $($Created | ConvertTo-Json -Compress)"
  }

  for ($i = 0; $i -lt 3; ++$i) {
    $ProduceOutput = & $Producer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Phase8Topic --key "$i" --message "message-00000000000000000000000000000000"
    if ($LASTEXITCODE -ne 0) {
      throw "Live Phase 8 produce failed with exit code $LASTEXITCODE. Output: $($ProduceOutput -join "`n")"
    }
    $Produced = ($ProduceOutput -join "`n") | ConvertFrom-Json
    if ($Produced.offset -ne $i) {
      throw "Expected Phase 8 offset $i, got $($Produced | ConvertTo-Json -Compress)"
    }
  }

  $Phase8AgeScript = @"
#!/usr/bin/env bash
set -euo pipefail
PART_DIR="/var/lib/boltstream/topics/$Phase8Topic/partition-000000"
echo "phase8 before retention:"
find "`$PART_DIR" -maxdepth 1 -type f -printf '%f %s bytes\n' | sort
mapfile -t logs < <(find "`$PART_DIR" -maxdepth 1 -name '*.log' | sort)
if [ "`${#logs[@]}" -lt 3 ]; then
  echo "expected at least three log segments, found `${#logs[@]}" >&2
  exit 1
fi
for ((i=0; i<`${#logs[@]}-1; ++i)); do
    sudo touch -d '8 days ago' "`${logs[`$i]}"
done
"@
  $Phase8AgeLocal = Join-Path $env:TEMP "boltstream-phase8-age.sh"
  $Phase8AgeRemote = "/tmp/boltstream-phase8-age.sh"
  [System.IO.File]::WriteAllText($Phase8AgeLocal, $Phase8AgeScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $Phase8AgeLocal "${InstanceName}:$Phase8AgeRemote"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy Phase 8 aging script to $InstanceName." }
  $AgeOutput = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $Phase8AgeRemote"
  if ($LASTEXITCODE -ne 0) { throw "Failed to age Phase 8 segments on $InstanceName." }
  $AgeOutput
  Remove-Item -Force -LiteralPath $Phase8AgeLocal -ErrorAction SilentlyContinue

  $RetentionOutput = & $Admin retention run --host $ExternalIp --port 9000 --token $BrokerToken --topic $Phase8Topic
  if ($LASTEXITCODE -ne 0) {
    throw "Live Phase 8 retention failed with exit code $LASTEXITCODE. Output: $($RetentionOutput -join "`n")"
  }
  $Retained = ($RetentionOutput -join "`n") | ConvertFrom-Json
  if ($Retained.segments_deleted -lt 2 -or $Retained.partitions[0].earliest_offset -ne 2) {
    throw "Unexpected live Phase 8 retention output: $($Retained | ConvertTo-Json -Compress)"
  }

  $TooOldOutput = & $Consumer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Phase8Topic --from 0
  if ($LASTEXITCODE -eq 0) {
    throw "Live Phase 8 retained-away fetch unexpectedly succeeded. Output: $($TooOldOutput -join "`n")"
  }
  $TooOld = ($TooOldOutput -join "`n") | ConvertFrom-Json
  if ($TooOld.error_code -ne "offset_out_of_range") {
    throw "Expected live Phase 8 offset_out_of_range, got $($TooOld | ConvertTo-Json -Compress)"
  }

  $BeginningOutput = & $Consumer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Phase8Topic --from beginning
  if ($LASTEXITCODE -ne 0) {
    throw "Live Phase 8 beginning fetch failed with exit code $LASTEXITCODE. Output: $($BeginningOutput -join "`n")"
  }
  $Beginning = ($BeginningOutput -join "`n") | ConvertFrom-Json
  if ($Beginning.from -ne 2 -or $Beginning.records[0].offset -ne 2) {
    throw "Unexpected live Phase 8 beginning fetch output: $($Beginning | ConvertTo-Json -Compress)"
  }

  $CommitOutput = & $Consumer --host $ExternalIp --port 9000 --token $BrokerToken --topic $Phase8Topic --group livephase8 --from beginning --commit
  if ($LASTEXITCODE -ne 0) {
    throw "Live Phase 8 commit failed with exit code $LASTEXITCODE. Output: $($CommitOutput -join "`n")"
  }

  $GroupOutput = & $Admin groups describe --host $ExternalIp --port 9000 --token $BrokerToken --group livephase8 --topic $Phase8Topic
  if ($LASTEXITCODE -ne 0) {
    throw "Live Phase 8 group describe failed with exit code $LASTEXITCODE. Output: $($GroupOutput -join "`n")"
  }
  $Group = ($GroupOutput -join "`n") | ConvertFrom-Json
  if (-not $Group.offsets[0].has_committed_offset -or $Group.offsets[0].committed_offset -ne 3) {
    throw "Unexpected live Phase 8 group output: $($Group | ConvertTo-Json -Compress)"
  }

  $ResetOutput = & $Admin groups reset-offset --host $ExternalIp --port 9000 --token $BrokerToken --group livephase8 --topic $Phase8Topic --partition 0 --to beginning
  if ($LASTEXITCODE -ne 0) {
    throw "Live Phase 8 group reset failed with exit code $LASTEXITCODE. Output: $($ResetOutput -join "`n")"
  }
  $Reset = ($ResetOutput -join "`n") | ConvertFrom-Json
  if ($Reset.next_offset -ne 2) {
    throw "Unexpected live Phase 8 reset output: $($Reset | ConvertTo-Json -Compress)"
  }

  $DeleteOutput = & $Admin topics delete --host $ExternalIp --port 9000 --token $BrokerToken --topic $Phase8Topic
  if ($LASTEXITCODE -ne 0) {
    throw "Live Phase 8 delete failed with exit code $LASTEXITCODE. Output: $($DeleteOutput -join "`n")"
  }
  $Deleted = ($DeleteOutput -join "`n") | ConvertFrom-Json
  if ($Deleted.status -ne "deleted") {
    throw "Unexpected live Phase 8 delete output: $($Deleted | ConvertTo-Json -Compress)"
  }

  $Phase8InspectScript = @"
#!/usr/bin/env bash
set -euo pipefail
echo "phase8 after delete topic path:"
if [ -e "/var/lib/boltstream/topics/$Phase8Topic" ]; then
  find "/var/lib/boltstream/topics/$Phase8Topic" -maxdepth 4 -type f -print
  exit 1
fi
echo "deleted"
echo "phase8 journal:"
journalctl -u boltstream.service -n 80 --no-pager | grep -E 'retention_applied|topic_deleted|protocol_error' || true
"@
  $Phase8InspectLocal = Join-Path $env:TEMP "boltstream-phase8-inspect.sh"
  $Phase8InspectRemote = "/tmp/boltstream-phase8-inspect.sh"
  [System.IO.File]::WriteAllText($Phase8InspectLocal, $Phase8InspectScript.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $Phase8InspectLocal "${InstanceName}:$Phase8InspectRemote"
  if ($LASTEXITCODE -ne 0) { throw "Failed to copy Phase 8 inspect script to $InstanceName." }
  $Phase8InspectOutput = & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $Phase8InspectRemote"
  if ($LASTEXITCODE -ne 0) { throw "Live Phase 8 remote inspection failed on $InstanceName." }
  $Phase8InspectOutput
  Remove-Item -Force -LiteralPath $Phase8InspectLocal -ErrorAction SilentlyContinue
  Write-Host "Live Phase 8 retention, group reset, and topic delete succeeded for topic $Phase8Topic."
} finally {
  & $Gcloud compute scp --strict-host-key-checking=no --project $ProjectId --zone $Zone $Phase8CleanupLocal "${InstanceName}:$Phase8CleanupRemote" | Out-Null
  if ($LASTEXITCODE -eq 0) {
    & $Gcloud compute ssh $InstanceName --strict-host-key-checking=no --project $ProjectId --zone $Zone --command "bash $Phase8CleanupRemote" | Out-Null
  }
  Remove-Item -Force -LiteralPath $Phase8OverrideLocal, $Phase8CleanupLocal -ErrorAction SilentlyContinue
}

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
echo "topics:"
find /var/lib/boltstream/topics -maxdepth 4 -type f -print 2>/dev/null || true
echo "consumer offsets:"
find /var/lib/boltstream/consumer_offsets -maxdepth 3 -type f -print -exec tail -n 5 {} \; 2>/dev/null || true
echo "journal:"
ACTIVE_SINCE="$(systemctl show -p ActiveEnterTimestamp --value boltstream.service)"
journalctl -u boltstream.service --since "${ACTIVE_SINCE}" -n 80 --no-pager
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
