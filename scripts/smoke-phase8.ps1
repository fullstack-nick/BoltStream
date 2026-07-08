param(
  [string]$Preset = "windows-gcc-debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-FreePort {
  $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
  $listener.Start()
  try {
    return $listener.LocalEndpoint.Port
  } finally {
    $listener.Stop()
  }
}

function Get-ToolPath {
  param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Name
  )

  $exe = Join-Path $BuildDir "$Name.exe"
  if (Test-Path $exe) { return $exe }
  $plain = Join-Path $BuildDir $Name
  if (Test-Path $plain) { return $plain }
  throw "Required binary not found: $exe"
}

function Wait-Ready {
  param([Parameter(Mandatory = $true)][int]$AdminPort)

  $deadline = [DateTimeOffset]::UtcNow.AddSeconds(10)
  do {
    try {
      curl.exe -fsS "http://127.0.0.1:$AdminPort/health/ready" | Out-Null
      return
    } catch {
      Start-Sleep -Milliseconds 200
    }
  } while ([DateTimeOffset]::UtcNow -lt $deadline)

  curl.exe -fsS "http://127.0.0.1:$AdminPort/health/ready" | Out-Null
}

function Stop-ProcessIfRunning {
  param([object]$Process)

  if ($Process -and -not $Process.HasExited) {
    Stop-Process -Id $Process.Id -Force
    $Process.WaitForExit()
  }
}

function Invoke-JsonTool {
  param(
    [Parameter(Mandatory = $true)][string]$Tool,
    [Parameter(Mandatory = $true)][string[]]$ToolArgs
  )

  $output = & $Tool @ToolArgs
  if ($LASTEXITCODE -ne 0) {
    throw "$Tool exited with $LASTEXITCODE for args '$($ToolArgs -join ' ')'. Output: $($output -join "`n")"
  }
  return (($output -join "`n") | ConvertFrom-Json)
}

function Invoke-FailingJsonTool {
  param(
    [Parameter(Mandatory = $true)][string]$Tool,
    [Parameter(Mandatory = $true)][string[]]$ToolArgs
  )

  $output = & $Tool @ToolArgs
  if ($LASTEXITCODE -eq 0) {
    throw "$Tool unexpectedly succeeded for args '$($ToolArgs -join ' ')'. Output: $($output -join "`n")"
  }
  return (($output -join "`n") | ConvertFrom-Json)
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$Server = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-server"
$Admin = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-admin"
$Producer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-producer"
$Consumer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-consumer"

$HadBrokerToken = Test-Path Env:\BOLTSTREAM_BROKER_TOKEN
$PreviousBrokerToken = $env:BOLTSTREAM_BROKER_TOKEN
$env:BOLTSTREAM_BROKER_TOKEN = "phase8-smoke-token-$([Guid]::NewGuid())"

$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase8-smoke-$([Guid]::NewGuid())"
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

$serverProcess = $null
try {
  $BrokerPort = Get-FreePort
  $AdminPort = Get-FreePort
  $DataDir = Join-Path $TempRoot "data"
  $Stdout = Join-Path $TempRoot "server.out"
  $Stderr = Join-Path $TempRoot "server.err"
  New-Item -ItemType Directory -Force -Path $DataDir | Out-Null

  $serverProcess = Start-Process `
    -FilePath $Server `
    -ArgumentList @(
      "--listen", "127.0.0.1:$BrokerPort",
      "--admin-listen", "127.0.0.1:$AdminPort",
      "--data", $DataDir,
      "--segment-bytes", "96",
      "--segment-max-age-seconds", "0",
      "--retention-max-age-seconds", "1",
      "--retention-max-bytes", "0",
      "--retention-check-interval-ms", "0"
    ) `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $Stdout `
    -RedirectStandardError $Stderr
  Wait-Ready -AdminPort $AdminPort

  $topic = "phase8"
  $created = Invoke-JsonTool $Admin @(
    "topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", $topic, "--partitions", "1"
  )
  if ($created.status -ne "created" -or $created.partitions -ne 1) {
    throw "unexpected create-topic output: $($created | ConvertTo-Json -Compress)"
  }

  for ($i = 0; $i -lt 3; ++$i) {
    $produced = Invoke-JsonTool $Producer @(
      "--host", "127.0.0.1", "--port", "$BrokerPort",
      "--topic", $topic, "--key", "$i",
      "--message", "message-00000000000000000000000000000000"
    )
    if ($produced.offset -ne $i) {
      throw "expected offset $i, got $($produced | ConvertTo-Json -Compress)"
    }
  }

  $partitionDir = Join-Path $DataDir "topics\$topic\partition-000000"
  $logs = @(Get-ChildItem -LiteralPath $partitionDir -Filter "*.log" | Sort-Object Name)
  if ($logs.Count -lt 3) {
    throw "expected at least three segment logs before retention, found $($logs.Count)"
  }
  for ($i = 0; $i -lt ($logs.Count - 1); ++$i) {
    $logs[$i].LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds(-10)
  }

  $retained = Invoke-JsonTool $Admin @(
    "retention", "run", "--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic
  )
  if ($retained.segments_deleted -lt 2 -or $retained.partitions[0].earliest_offset -ne 2) {
    throw "unexpected retention output: $($retained | ConvertTo-Json -Compress)"
  }

  $tooOld = Invoke-FailingJsonTool $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic, "--from", "0"
  )
  if ($tooOld.error_code -ne "offset_out_of_range") {
    throw "expected offset_out_of_range, got $($tooOld | ConvertTo-Json -Compress)"
  }

  $beginning = Invoke-JsonTool $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic, "--from", "beginning"
  )
  if ($beginning.from -ne 2 -or $beginning.count -ne 1 -or $beginning.records[0].offset -ne 2) {
    throw "unexpected beginning fetch output: $($beginning | ConvertTo-Json -Compress)"
  }

  $commit = Invoke-JsonTool $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic,
    "--group", "dashboard", "--from", "beginning", "--commit"
  )
  if ($commit.committed_offset -ne 3) {
    throw "unexpected commit output: $($commit | ConvertTo-Json -Compress)"
  }

  $group = Invoke-JsonTool $Admin @(
    "groups", "describe", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--group", "dashboard", "--topic", $topic
  )
  if (-not $group.offsets[0].has_committed_offset -or $group.offsets[0].committed_offset -ne 3) {
    throw "unexpected group describe output: $($group | ConvertTo-Json -Compress)"
  }

  $reset = Invoke-JsonTool $Admin @(
    "groups", "reset-offset", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--group", "dashboard", "--topic", $topic, "--partition", "0", "--to", "beginning"
  )
  if ($reset.next_offset -ne 2) {
    throw "unexpected reset output: $($reset | ConvertTo-Json -Compress)"
  }

  $deleted = Invoke-JsonTool $Admin @(
    "topics", "delete", "--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", $topic
  )
  if ($deleted.status -ne "deleted" -or (Test-Path (Join-Path $DataDir "topics\$topic"))) {
    throw "unexpected delete output or topic directory remains: $($deleted | ConvertTo-Json -Compress)"
  }

  Write-Host "Phase 8 retention and topic lifecycle smoke passed."
  exit 0
} finally {
  Stop-ProcessIfRunning -Process $serverProcess
  Remove-Item -Recurse -Force -LiteralPath $TempRoot -ErrorAction SilentlyContinue
  if ($HadBrokerToken) {
    $env:BOLTSTREAM_BROKER_TOKEN = $PreviousBrokerToken
  } else {
    Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue
  }
}
