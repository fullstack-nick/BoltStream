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
    [Parameter(Mandatory = $true)][string]$Stderr,
    [string[]]$ExtraArgs = @()
  )

  $arguments = @(
    "--listen", "127.0.0.1:$BrokerPort",
    "--admin-listen", "127.0.0.1:$AdminPort",
    "--data", $DataDir
  ) + $ExtraArgs

  $process = Start-Process `
    -FilePath $Server `
    -ArgumentList $arguments `
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

function Invoke-JsonToolExpectExit {
  param(
    [Parameter(Mandatory = $true)][string]$Tool,
    [Parameter(Mandatory = $true)][string[]]$ToolArgs,
    [Parameter(Mandatory = $true)][int]$ExpectedExit
  )

  $output = & $Tool @ToolArgs
  if ($LASTEXITCODE -ne $ExpectedExit) {
    throw "$Tool exited with $LASTEXITCODE, expected $ExpectedExit for args '$($ToolArgs -join ' ')'. Output: $($output -join "`n")"
  }
  return (($output -join "`n") | ConvertFrom-Json)
}

function Assert-LogContains {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Pattern
  )

  $content = if (Test-Path $Path) { Get-Content -Raw $Path } else { "" }
  if ($content -notmatch $Pattern) {
    throw "Expected log '$Path' to match pattern '$Pattern'. Content: $content"
  }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$Server = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-server"
$Admin = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-admin"
$Producer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-producer"
$Consumer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-consumer"

$HadBrokerToken = Test-Path Env:\BOLTSTREAM_BROKER_TOKEN
$PreviousBrokerToken = $env:BOLTSTREAM_BROKER_TOKEN
$env:BOLTSTREAM_BROKER_TOKEN = "phase6-smoke-token-$([Guid]::NewGuid())"

$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase6-smoke-$([Guid]::NewGuid())"
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

$serverProcess = $null
$longPollProcess = $null
try {
  $BrokerPort = Get-FreePort
  $AdminPort = Get-FreePort
  $DataDir = Join-Path $TempRoot "normal-data"
  $Stdout = Join-Path $TempRoot "normal.out"
  $Stderr = Join-Path $TempRoot "normal.err"
  $LongPollOut = Join-Path $TempRoot "longpoll.out"
  $LongPollErr = Join-Path $TempRoot "longpoll.err"
  New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
  $serverProcess = Start-BoltStreamServer $Server $BrokerPort $AdminPort $DataDir $Stdout $Stderr

  $created = Invoke-JsonTool $Admin @(
    "topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--partitions", "3"
  )
  if ($created.status -ne "created" -or $created.partitions -ne 3) {
    throw "unexpected create-topic output: $($created | ConvertTo-Json -Compress)"
  }

  $produced = Invoke-JsonTool $Producer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--key", "AAPL", "--message", "AAPL,100,192.41"
  )
  $consumed = Invoke-JsonTool $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--partition", "$($produced.partition)", "--from", "beginning",
    "--group", "dashboard", "--commit"
  )
  if ($consumed.status -ne "ok" -or $consumed.count -ne 1 -or $consumed.committed_offset -ne 1) {
    throw "unexpected committed consume output: $($consumed | ConvertTo-Json -Compress)"
  }

  $resumed = Invoke-JsonTool $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--partition", "$($produced.partition)", "--group", "dashboard"
  )
  if ($resumed.status -ne "ok" -or $resumed.from -ne 1 -or $resumed.count -ne 0) {
    throw "unexpected committed resume output: $($resumed | ConvertTo-Json -Compress)"
  }

  $longPollProcess = Start-Process `
    -FilePath $Consumer `
    -ArgumentList @(
      "--host", "127.0.0.1", "--port", "$BrokerPort",
      "--topic", "trades", "--partition", "$($produced.partition)",
      "--from", "latest", "--wait-ms", "5000", "--timeout-ms", "8000"
    ) `
    -PassThru `
    -WindowStyle Hidden `
    -RedirectStandardOutput $LongPollOut `
    -RedirectStandardError $LongPollErr

  Start-Sleep -Milliseconds 500
  Invoke-JsonTool $Producer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "trades", "--key", "AAPL", "--message", "long-poll-release"
  ) | Out-Null
  if (-not $longPollProcess.WaitForExit(8000)) {
    throw "long-poll consumer did not exit after delayed produce"
  }
  $longPoll = (Get-Content -Raw $LongPollOut) | ConvertFrom-Json
  if ($longPoll.status -ne "ok" -or $longPoll.count -ne 1) {
    throw "unexpected long-poll output: $($longPoll | ConvertTo-Json -Compress)"
  }
  Stop-BoltStreamServer -Process $serverProcess
  $serverProcess = $null
  Assert-LogContains $Stderr '"event":"protocol_request"'
  Assert-LogContains $Stderr '"correlation_id":'
  Assert-LogContains $Stderr '"event":"protocol_response"'

  $BrokerPort = Get-FreePort
  $AdminPort = Get-FreePort
  $DataDir = Join-Path $TempRoot "append-overload-data"
  $Stdout = Join-Path $TempRoot "append-overload.out"
  $Stderr = Join-Path $TempRoot "append-overload.err"
  New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
  $serverProcess = Start-BoltStreamServer $Server $BrokerPort $AdminPort $DataDir $Stdout $Stderr @(
    "--max-append-queue-depth", "0"
  )
  Invoke-JsonTool $Admin @(
    "topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "overload", "--partitions", "1"
  ) | Out-Null
  $appendOverload = Invoke-JsonToolExpectExit $Producer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "overload", "--message", "blocked"
  ) 5
  if ($appendOverload.status -ne "overloaded" -or $appendOverload.retryable -ne $true) {
    throw "unexpected append overload output: $($appendOverload | ConvertTo-Json -Compress)"
  }
  Stop-BoltStreamServer -Process $serverProcess
  $serverProcess = $null
  Assert-LogContains $Stderr '"event":"append_overloaded"'

  $BrokerPort = Get-FreePort
  $AdminPort = Get-FreePort
  $DataDir = Join-Path $TempRoot "waiter-overload-data"
  $Stdout = Join-Path $TempRoot "waiter-overload.out"
  $Stderr = Join-Path $TempRoot "waiter-overload.err"
  New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
  $serverProcess = Start-BoltStreamServer $Server $BrokerPort $AdminPort $DataDir $Stdout $Stderr @(
    "--max-long-poll-waiters", "0"
  )
  Invoke-JsonTool $Admin @(
    "topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "waitlimit", "--partitions", "1"
  ) | Out-Null
  $waiterOverload = Invoke-JsonToolExpectExit $Consumer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "waitlimit", "--from", "latest", "--wait-ms", "1000"
  ) 5
  if ($waiterOverload.status -ne "overloaded" -or $waiterOverload.retryable -ne $true) {
    throw "unexpected waiter overload output: $($waiterOverload | ConvertTo-Json -Compress)"
  }
  Stop-BoltStreamServer -Process $serverProcess
  $serverProcess = $null
  Assert-LogContains $Stderr '"event":"long_poll_overloaded"'

  $BrokerPort = Get-FreePort
  $AdminPort = Get-FreePort
  $DataDir = Join-Path $TempRoot "frame-limit-data"
  $Stdout = Join-Path $TempRoot "frame-limit.out"
  $Stderr = Join-Path $TempRoot "frame-limit.err"
  New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
  $serverProcess = Start-BoltStreamServer $Server $BrokerPort $AdminPort $DataDir $Stdout $Stderr @(
    "--max-frame-bytes", "96"
  )
  Invoke-JsonTool $Admin @(
    "topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "frames", "--partitions", "1"
  ) | Out-Null
  $frameLimit = Invoke-JsonToolExpectExit $Producer @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", "frames", "--message", ("x" * 128)
  ) 1
  if ($frameLimit.status -ne "invalid_length" -or $frameLimit.retryable -ne $false) {
    throw "unexpected frame-limit output: $($frameLimit | ConvertTo-Json -Compress)"
  }
  Stop-BoltStreamServer -Process $serverProcess
  $serverProcess = $null
  Assert-LogContains $Stderr '"error_code":"invalid_length"'

  Write-Host "Phase 6 backpressure/robustness smoke passed."
} finally {
  if ($longPollProcess -and -not $longPollProcess.HasExited) {
    Stop-Process -Id $longPollProcess.Id -Force
    $longPollProcess.WaitForExit()
  }
  Stop-BoltStreamServer -Process $serverProcess
  Remove-Item -Recurse -Force -LiteralPath $TempRoot -ErrorAction SilentlyContinue
  if ($HadBrokerToken) {
    $env:BOLTSTREAM_BROKER_TOKEN = $PreviousBrokerToken
  } else {
    Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue
  }
}
