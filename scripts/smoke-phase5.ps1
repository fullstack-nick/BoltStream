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

function Start-BoltStreamServer {
  param(
    [Parameter(Mandatory = $true)][string]$Server,
    [Parameter(Mandatory = $true)][int]$BrokerPort,
    [Parameter(Mandatory = $true)][int]$AdminPort,
    [Parameter(Mandatory = $true)][string]$DataDir,
    [Parameter(Mandatory = $true)][string]$Stdout,
    [Parameter(Mandatory = $true)][string]$Stderr
  )

  $process = Start-Process `
    -FilePath $Server `
    -ArgumentList @(
      "--listen", "127.0.0.1:$BrokerPort",
      "--admin-listen", "127.0.0.1:$AdminPort",
      "--data", $DataDir
    ) `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $Stdout `
    -RedirectStandardError $Stderr

  Wait-Ready -AdminPort $AdminPort
  return $process
}

function Stop-BoltStreamServer {
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

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$Server = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-server"
$Admin = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-admin"
$Producer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-producer"
$Consumer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-consumer"

$BrokerPort = Get-FreePort
$AdminPort = Get-FreePort
$DataDir = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase5-smoke-$([Guid]::NewGuid())"
$Stdout = Join-Path $env:TEMP "boltstream-phase5-smoke.out"
$Stderr = Join-Path $env:TEMP "boltstream-phase5-smoke.err"
$LongPollOut = Join-Path $env:TEMP "boltstream-phase5-longpoll.out"
$LongPollErr = Join-Path $env:TEMP "boltstream-phase5-longpoll.err"
$HadBrokerToken = Test-Path Env:\BOLTSTREAM_BROKER_TOKEN
$PreviousBrokerToken = $env:BOLTSTREAM_BROKER_TOKEN
$env:BOLTSTREAM_BROKER_TOKEN = "phase5-smoke-token-$([Guid]::NewGuid())"
Remove-Item -Recurse -Force -LiteralPath $DataDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
Remove-Item -Force -LiteralPath $Stdout, $Stderr, $LongPollOut, $LongPollErr -ErrorAction SilentlyContinue

$serverProcess = $null
$longPollProcess = $null
try {
  $serverProcess = Start-BoltStreamServer `
    -Server $Server `
    -BrokerPort $BrokerPort `
    -AdminPort $AdminPort `
    -DataDir $DataDir `
    -Stdout $Stdout `
    -Stderr $Stderr

  $created = Invoke-JsonTool $Admin @(
    "topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--partitions", "3"
  )
  if ($created.status -ne "created" -or $created.partitions -ne 3) {
    throw "unexpected create-topic output: $($created | ConvertTo-Json -Compress)"
  }

  $partitions = @()
  foreach ($index in 0..2) {
    $produced = Invoke-JsonTool $Producer @(
      "--host", "127.0.0.1", "--port", "$BrokerPort",
      "--topic", "trades", "--message", "round-robin-$index"
    )
    $partitions += [int]$produced.partition
  }
  if (($partitions -join ",") -ne "0,1,2") {
    throw "unexpected round-robin partitions: $($partitions -join ',')"
  }

  $consumed = Invoke-JsonTool $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--partition", "0", "--group", "dashboard", "--commit"
  )
  if ($consumed.status -ne "ok" -or $consumed.count -ne 1 -or $consumed.committed_offset -ne 1) {
    throw "unexpected committed consume output: $($consumed | ConvertTo-Json -Compress)"
  }

  Stop-BoltStreamServer -Process $serverProcess
  $serverProcess = $null

  $serverProcess = Start-BoltStreamServer `
    -Server $Server `
    -BrokerPort $BrokerPort `
    -AdminPort $AdminPort `
    -DataDir $DataDir `
    -Stdout $Stdout `
    -Stderr $Stderr

  $resumed = Invoke-JsonTool $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--partition", "0", "--group", "dashboard"
  )
  if ($resumed.status -ne "ok" -or $resumed.from -ne 1 -or $resumed.count -ne 0) {
    throw "unexpected committed resume output: $($resumed | ConvertTo-Json -Compress)"
  }

  $longPollProcess = Start-Process `
    -FilePath $Consumer `
    -ArgumentList @(
      "--host", "127.0.0.1", "--port", "$BrokerPort",
      "--topic", "trades", "--partition", "0", "--from", "latest",
      "--wait-ms", "5000", "--timeout-ms", "8000"
    ) `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $LongPollOut `
    -RedirectStandardError $LongPollErr

  Start-Sleep -Milliseconds 500
  $longPollProduce = Invoke-JsonTool $Producer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--message", "long-poll-release"
  )
  if ($longPollProduce.partition -ne 0) {
    throw "expected long-poll release record on partition 0, got partition $($longPollProduce.partition)"
  }

  if (-not $longPollProcess.WaitForExit(8000)) {
    throw "long-poll consumer did not exit after delayed produce"
  }
  $longPollProcess.WaitForExit()
  $longPollProcess.Refresh()
  if ($null -ne $longPollProcess.ExitCode -and $longPollProcess.ExitCode -ne 0) {
    $err = if (Test-Path $LongPollErr) { Get-Content -Raw $LongPollErr } else { "" }
    throw "long-poll consumer exited with $($longPollProcess.ExitCode): $err"
  }
  $longPoll = (Get-Content -Raw $LongPollOut) | ConvertFrom-Json
  if ($longPoll.status -ne "ok" -or $longPoll.count -ne 1 -or
      $longPoll.records[0].message -ne "long-poll-release") {
    throw "unexpected long-poll output: $($longPoll | ConvertTo-Json -Compress)"
  }

  Write-Host "Phase 5 multi-partition/group/long-poll smoke passed on 127.0.0.1:$BrokerPort with data dir $DataDir."
} finally {
  if ($longPollProcess -and -not $longPollProcess.HasExited) {
    Stop-Process -Id $longPollProcess.Id -Force
    $longPollProcess.WaitForExit()
  }
  Stop-BoltStreamServer -Process $serverProcess
  Remove-Item -Recurse -Force -LiteralPath $DataDir -ErrorAction SilentlyContinue
  Remove-Item -Force -LiteralPath $LongPollOut, $LongPollErr -ErrorAction SilentlyContinue
  if ($HadBrokerToken) {
    $env:BOLTSTREAM_BROKER_TOKEN = $PreviousBrokerToken
  } else {
    Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue
  }
}
