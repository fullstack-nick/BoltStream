param(
  [string]$Preset = "windows-gcc-debug",
  [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-FreePort {
  $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
  $listener.Start()
  try { return $listener.LocalEndpoint.Port } finally { $listener.Stop() }
}

function Get-ToolPath([string]$BuildDir, [string]$Name) {
  foreach ($candidate in @((Join-Path $BuildDir "$Name.exe"), (Join-Path $BuildDir $Name))) {
    if (Test-Path -LiteralPath $candidate) { return $candidate }
  }
  throw "Required binary not found in ${BuildDir}: $Name"
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build/$Preset"
if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot "build.ps1") -Preset $Preset
  if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
}

$Server = Get-ToolPath $BuildDir "boltstream-server"
$BrokerPort = Get-FreePort
$AdminPort = Get-FreePort
$DataDir = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-polish-$([Guid]::NewGuid())"
$Stdout = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-polish-$([Guid]::NewGuid()).out"
$Stderr = Join-Path ([System.IO.Path]::GetTempPath()) "boltstream-polish-$([Guid]::NewGuid()).err"
$Token = "python-client-$([Guid]::NewGuid())"
$PreviousToken = $env:BOLTSTREAM_BROKER_TOKEN
$HadToken = Test-Path Env:\BOLTSTREAM_BROKER_TOKEN
$Process = $null

New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
try {
  $env:BOLTSTREAM_BROKER_TOKEN = $Token
  $StartParameters = @{
    FilePath = $Server
    ArgumentList = @(
      "--listen", "127.0.0.1:$BrokerPort", "--admin-listen", "127.0.0.1:$AdminPort",
      "--data", $DataDir
    )
    PassThru = $true
    RedirectStandardOutput = $Stdout
    RedirectStandardError = $Stderr
  }
  if ($env:OS -eq "Windows_NT") { $StartParameters.WindowStyle = "Hidden" }
  $Process = Start-Process @StartParameters

  $Deadline = [DateTimeOffset]::UtcNow.AddSeconds(15)
  do {
    try {
      Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$AdminPort/health/ready" | Out-Null
      break
    } catch {
      Start-Sleep -Milliseconds 200
    }
  } while ([DateTimeOffset]::UtcNow -lt $Deadline)
  if ([DateTimeOffset]::UtcNow -ge $Deadline) { throw "broker did not become ready" }

  $Client = Join-Path $RepoRoot "clients/python/boltstream_client.py"
  $Output = & python $Client --host 127.0.0.1 --port $BrokerPort --token $Token demo `
    --topic python-demo --message "interoperability-ok"
  if ($LASTEXITCODE -ne 0) { throw "Python client exited with $LASTEXITCODE" }
  $Result = ($Output -join "`n") | ConvertFrom-Json
  if ($Result.created.status -ne "created" -or $Result.produced.offset -ne 0 -or
      $Result.produced.next_offset -ne 1 -or $Result.fetched.records.Count -ne 1 -or
      $Result.fetched.records[0].key -ne "python" -or
      $Result.fetched.records[0].message -ne "interoperability-ok") {
    throw "unexpected Python interoperability result: $($Result | ConvertTo-Json -Depth 8 -Compress)"
  }
  Write-Host "Python interoperability smoke passed: authenticated create, produce, and fetch."
} finally {
  if ($Process -and -not $Process.HasExited) {
    Stop-Process -Id $Process.Id -Force
    $Process.WaitForExit()
  }
  Remove-Item -Recurse -Force -LiteralPath $DataDir -ErrorAction SilentlyContinue
  Remove-Item -Force -LiteralPath $Stdout, $Stderr -ErrorAction SilentlyContinue
  if ($HadToken) { $env:BOLTSTREAM_BROKER_TOKEN = $PreviousToken } else {
    Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue
  }
}
