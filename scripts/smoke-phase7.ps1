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

function Wait-FileContains {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Text,
    [int]$TimeoutMs = 10000
  )

  $deadline = [DateTimeOffset]::UtcNow.AddMilliseconds($TimeoutMs)
  do {
    $content = ""
    if (Test-Path $Path) {
      $raw = Get-Content -Raw $Path
      if ($null -ne $raw) { $content = [string]$raw }
    }
    if ($content.Contains($Text)) { return }
    Start-Sleep -Milliseconds 100
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
    [int]$TimeoutMs = 10000
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
    Start-Sleep -Milliseconds 100
  } while ([DateTimeOffset]::UtcNow -lt $deadline)

  $content = ""
  if (Test-Path $Path) {
    $raw = Get-Content -Raw $Path
    if ($null -ne $raw) { $content = [string]$raw }
  }
  throw "Timed out waiting for '$Text' count $MinimumCount in '$Path'. Content: $content"
}

function Assert-LogContains {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Pattern
  )

  $content = ""
  if (Test-Path $Path) {
    $raw = Get-Content -Raw $Path
    if ($null -ne $raw) { $content = [string]$raw }
  }
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
$env:BOLTSTREAM_BROKER_TOKEN = "phase7-smoke-token-$([Guid]::NewGuid())"

$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-phase7-smoke-$([Guid]::NewGuid())"
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

$serverProcess = $null
$consumer1 = $null
$consumer2 = $null
try {
  $BrokerPort = Get-FreePort
  $AdminPort = Get-FreePort
  $DataDir = Join-Path $TempRoot "data"
  $Stdout = Join-Path $TempRoot "server.out"
  $Stderr = Join-Path $TempRoot "server.err"
  $Consumer1Out = Join-Path $TempRoot "consumer1.out"
  $Consumer1Err = Join-Path $TempRoot "consumer1.err"
  $Consumer2Out = Join-Path $TempRoot "consumer2.out"
  $Consumer2Err = Join-Path $TempRoot "consumer2.err"
  New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
  $serverProcess = Start-BoltStreamServer $Server $BrokerPort $AdminPort $DataDir $Stdout $Stderr

  $topic = "phase7"
  $created = Invoke-JsonTool $Admin @(
    "topics", "create", "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", $topic, "--partitions", "4"
  )
  if ($created.status -ne "created" -or $created.partitions -ne 4) {
    throw "unexpected create-topic output: $($created | ConvertTo-Json -Compress)"
  }

  $baseConsumerArgs = @(
    "--host", "127.0.0.1", "--port", "$BrokerPort",
    "--topic", $topic, "--group", "dashboard", "--commit", "--coordinated",
    "--session-timeout-ms", "1200", "--heartbeat-ms", "300", "--poll-ms", "100",
    "--idle-exit-ms", "12000", "--timeout-ms", "5000"
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

  Wait-FileContains $Consumer1Out '"partitions":[0,1]' 10000
  Wait-FileContains $Consumer2Out '"partitions":[2,3]' 10000

  for ($i = 0; $i -lt 4; ++$i) {
    $produced = Invoke-JsonTool $Producer @(
      "--host", "127.0.0.1", "--port", "$BrokerPort",
      "--topic", $topic, "--message", "share-$i"
    )
    if ($produced.partition -ne $i) {
      throw "expected share-$i on partition $i, got $($produced | ConvertTo-Json -Compress)"
    }
  }

  Wait-FileContains $Consumer1Out '"message":"share-0"' 10000
  Wait-FileContains $Consumer1Out '"message":"share-1"' 10000
  Wait-FileContains $Consumer2Out '"message":"share-2"' 10000
  Wait-FileContains $Consumer2Out '"message":"share-3"' 10000

  Stop-ProcessIfRunning -Process $consumer2
  $consumer2 = $null
  Wait-FilePatternCount $Consumer1Out '"partitions":[0,1,2,3]' 2 10000

  for ($i = 0; $i -lt 4; ++$i) {
    $produced = Invoke-JsonTool $Producer @(
      "--host", "127.0.0.1", "--port", "$BrokerPort",
      "--topic", $topic, "--message", "takeover-$i"
    )
    if ($produced.partition -ne $i) {
      throw "expected takeover-$i on partition $i, got $($produced | ConvertTo-Json -Compress)"
    }
  }

  for ($i = 0; $i -lt 4; ++$i) {
    Wait-FileContains $Consumer1Out "`"message`":`"takeover-$i`"" 10000
  }

  if (-not $consumer1.WaitForExit(20000)) {
    throw "coordinated survivor did not exit after idle timeout"
  }
  $consumer1.Refresh()
  if ($null -ne $consumer1.ExitCode -and $consumer1.ExitCode -ne 0) {
    $err = if (Test-Path $Consumer1Err) { Get-Content -Raw $Consumer1Err } else { "" }
    throw "coordinated survivor exited with $($consumer1.ExitCode): $err"
  }
  Wait-FileContains $Consumer1Out '"event":"summary"' 1000
  $consumer1 = $null

  Assert-LogContains $Stderr '"event":"group_member_joined"'
  Assert-LogContains $Stderr '"event":"group_rebalanced"'
  Assert-LogContains $Stderr '"event":"group_member_expired"'
  Assert-LogContains $Stderr '"event":"group_offset_committed"'

  Write-Host "Phase 7 coordinated consumer group smoke passed."
  exit 0
} finally {
  Stop-ProcessIfRunning -Process $consumer2
  Stop-ProcessIfRunning -Process $consumer1
  Stop-ProcessIfRunning -Process $serverProcess
  Remove-Item -Recurse -Force -LiteralPath $TempRoot -ErrorAction SilentlyContinue
  if ($HadBrokerToken) {
    $env:BOLTSTREAM_BROKER_TOKEN = $PreviousBrokerToken
  } else {
    Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue
  }
}
