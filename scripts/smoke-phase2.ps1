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

function Invoke-ExpectNotImplemented {
  param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string[]]$Arguments,
    [Parameter(Mandatory = $true)][string]$Name
  )

  $output = & $Executable @Arguments 2>&1
  $exitCode = $LASTEXITCODE
  $text = $output -join "`n"
  if ($exitCode -ne 3) {
    throw "$Name exited with $exitCode, expected 3. Output: $text"
  }
  if ($text -notmatch '"status":"not_implemented"') {
    throw "$Name did not return structured not_implemented output. Output: $text"
  }
  Write-Host "$Name returned expected not_implemented response."
  $text
}

$BuildDir = Join-Path (Get-Location) "build\$Preset"
$Server = Join-Path $BuildDir "boltstream-server.exe"
$Producer = Join-Path $BuildDir "boltstream-producer.exe"
$Consumer = Join-Path $BuildDir "boltstream-consumer.exe"

foreach ($Path in @($Server, $Producer, $Consumer)) {
  if (-not (Test-Path $Path)) {
    throw "Required binary not found: $Path. Run scripts/build.ps1 first."
  }
}

$BrokerPort = Get-FreePort
$AdminPort = Get-FreePort
$DataDir = Join-Path (Get-Location) "data\phase2-smoke"
$Stdout = Join-Path $env:TEMP "boltstream-phase2-smoke.out"
$Stderr = Join-Path $env:TEMP "boltstream-phase2-smoke.err"
Remove-Item -Recurse -Force -LiteralPath $DataDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
Remove-Item -Force -LiteralPath $Stdout, $Stderr -ErrorAction SilentlyContinue

$ServerProcess = Start-Process `
  -FilePath $Server `
  -ArgumentList @("--listen", "127.0.0.1:$BrokerPort", "--admin-listen", "127.0.0.1:$AdminPort", "--data", $DataDir) `
  -PassThru `
  -WindowStyle Hidden `
  -RedirectStandardOutput $Stdout `
  -RedirectStandardError $Stderr

try {
  $deadline = [DateTimeOffset]::UtcNow.AddSeconds(10)
  do {
    try {
      curl.exe -fsS "http://127.0.0.1:$AdminPort/health/ready" | Out-Null
      break
    } catch {
      Start-Sleep -Milliseconds 200
    }
  } while ([DateTimeOffset]::UtcNow -lt $deadline)

  curl.exe -fsS "http://127.0.0.1:$AdminPort/health/ready" | Out-Null

  Invoke-ExpectNotImplemented `
    -Executable $Producer `
    -Name "producer" `
    -Arguments @("--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", "trades", "--key", "AAPL", "--message", "AAPL,100,192.41")

  Invoke-ExpectNotImplemented `
    -Executable $Consumer `
    -Name "consumer" `
    -Arguments @("--host", "127.0.0.1", "--port", "$BrokerPort", "--topic", "trades", "--from", "beginning")

  Write-Host "Phase 2 CLI smoke passed on 127.0.0.1:$BrokerPort."
} finally {
  if ($ServerProcess -and -not $ServerProcess.HasExited) {
    Stop-Process -Id $ServerProcess.Id -Force
    $ServerProcess.WaitForExit()
  }
}
