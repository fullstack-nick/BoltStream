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
  if (Test-Path $exe) {
    return $exe
  }

  $plain = Join-Path $BuildDir $Name
  if (Test-Path $plain) {
    return $plain
  }

  throw "Required binary not found: $exe"
}

function Wait-Ready {
  param(
    [Parameter(Mandatory = $true)][int]$AdminPort
  )

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

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$Server = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-server"
$Producer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-producer"
$Consumer = Get-ToolPath -BuildDir $BuildDir -Name "boltstream-consumer"

$BrokerPort = Get-FreePort
$AdminPort = Get-FreePort
$DataDir = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase4-smoke-$([Guid]::NewGuid())"
$Stdout = Join-Path $env:TEMP "boltstream-phase4-smoke.out"
$Stderr = Join-Path $env:TEMP "boltstream-phase4-smoke.err"
Remove-Item -Recurse -Force -LiteralPath $DataDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
Remove-Item -Force -LiteralPath $Stdout, $Stderr -ErrorAction SilentlyContinue

$serverProcess = $null
try {
  $serverProcess = Start-BoltStreamServer `
    -Server $Server `
    -BrokerPort $BrokerPort `
    -AdminPort $AdminPort `
    -DataDir $DataDir `
    -Stdout $Stdout `
    -Stderr $Stderr

  $produceOutput = & $Producer --host 127.0.0.1 --port $BrokerPort --topic trades --key AAPL --message "AAPL,100,192.41"
  if ($LASTEXITCODE -ne 0) {
    throw "producer exited with $LASTEXITCODE. Output: $($produceOutput -join "`n")"
  }
  $produced = ($produceOutput -join "`n") | ConvertFrom-Json
  if ($produced.status -ne "ok" -or $produced.offset -ne 0 -or $produced.next_offset -ne 1) {
    throw "unexpected producer output: $($produced | ConvertTo-Json -Compress)"
  }

  $fetchOutput = & $Consumer --host 127.0.0.1 --port $BrokerPort --topic trades --from beginning
  if ($LASTEXITCODE -ne 0) {
    throw "consumer exited with $LASTEXITCODE. Output: $($fetchOutput -join "`n")"
  }
  $fetched = ($fetchOutput -join "`n") | ConvertFrom-Json
  if ($fetched.status -ne "ok" -or $fetched.count -ne 1 -or $fetched.records[0].key -ne "AAPL") {
    throw "unexpected consumer output: $($fetched | ConvertTo-Json -Compress)"
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

  $restartFetchOutput = & $Consumer --host 127.0.0.1 --port $BrokerPort --topic trades --from 0
  if ($LASTEXITCODE -ne 0) {
    throw "consumer after restart exited with $LASTEXITCODE. Output: $($restartFetchOutput -join "`n")"
  }
  $restartFetched = ($restartFetchOutput -join "`n") | ConvertFrom-Json
  if ($restartFetched.status -ne "ok" -or $restartFetched.count -ne 1 -or
      $restartFetched.records[0].message -ne "AAPL,100,192.41" -or
      $restartFetched.next_offset -ne 1) {
    throw "unexpected restart consumer output: $($restartFetched | ConvertTo-Json -Compress)"
  }

  Write-Host "Phase 4 broker produce/fetch smoke passed on 127.0.0.1:$BrokerPort with data dir $DataDir."
} finally {
  Stop-BoltStreamServer -Process $serverProcess
  Remove-Item -Recurse -Force -LiteralPath $DataDir -ErrorAction SilentlyContinue
}
