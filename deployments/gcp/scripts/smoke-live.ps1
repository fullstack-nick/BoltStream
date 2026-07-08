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
    "--session-timeout-ms", "1200", "--heartbeat-ms", "300", "--poll-ms", "100",
    "--idle-exit-ms", "12000", "--timeout-ms", "7000"
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

  Stop-ProcessIfRunning -Process $consumer2
  $consumer2 = $null
  Wait-FilePatternCount $Consumer1Out '"partitions":[0,1,2,3]' 2

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

  if (-not $consumer1.WaitForExit(22000)) {
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
