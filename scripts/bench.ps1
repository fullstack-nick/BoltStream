param(
  [string]$Preset = "windows-msvc-release",
  [ValidateSet("all", "single-threaded", "worker-event-loops", "batched-writes")]
  [string]$Profile = "all",
  [string]$OutputDir = "artifacts/benchmarks/local",
  [switch]$Quick,
  [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $SkipBuild) {
  & "$PSScriptRoot\build.ps1" -Preset $Preset
}

$BuildDir = Join-Path (Get-Location) "build\$Preset"
$RunningOnWindows = $env:OS -eq "Windows_NT"
$ExecutableSuffix = if ($RunningOnWindows) { ".exe" } else { "" }
$Server = Join-Path $BuildDir "boltstream-server$ExecutableSuffix"
$Bench = Join-Path $BuildDir "boltstream-bench$ExecutableSuffix"
if (-not (Test-Path $Server) -or -not (Test-Path $Bench)) {
  throw "Phase 10 binaries were not found in $BuildDir."
}

$Profiles = [ordered]@{
  "single-threaded" = @{ Config = "benchmarks/profiles/single-threaded.yaml"; Io = 1; Append = 0; Batch = 1 }
  "worker-event-loops" = @{ Config = "benchmarks/profiles/worker-event-loops.yaml"; Io = 2; Append = 2; Batch = 1 }
  "batched-writes" = @{ Config = "benchmarks/profiles/batched-writes.yaml"; Io = 2; Append = 2; Batch = 32 }
}
if ($Profile -ne "all") {
  $Selected = [ordered]@{ $Profile = $Profiles[$Profile] }
  $Profiles = $Selected
}

$Duration = if ($Quick) { 1 } else { 30 }
$WarmupSeconds = if ($Quick) { 0 } else { 60 }
$Messages = if ($Quick) { 400 } else { 100000 }
$FetchMessages = if ($Quick) { 400 } else { 250000 }
$WarmupMessages = if ($Quick) { 20 } else { 10000 }
$Repetitions = if ($Quick) { 1 } else { 3 }
$Clients = if ($Quick) { 8 } else { 16 }
$EnvironmentKind = if ($RunningOnWindows) { "native-windows" } else { "native-linux" }
$MachineType = if ($RunningOnWindows) { "developer-laptop" } else { "developer-host" }
$Port = 19010
$AdminPort = 19110
$Token = "phase10-local-benchmark-token"
$PreviousToken = $env:BOLTSTREAM_BROKER_TOKEN
$HadToken = Test-Path Env:\BOLTSTREAM_BROKER_TOKEN
$env:BOLTSTREAM_BROKER_TOKEN = $Token
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

try {
  foreach ($Entry in $Profiles.GetEnumerator()) {
    $Name = $Entry.Key
    $Metadata = $Entry.Value
    $TempRoot = Join-Path $env:TEMP ("boltstream-phase10-$Name-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $TempRoot | Out-Null
    $Stdout = Join-Path $TempRoot "server.stdout.log"
    $Stderr = Join-Path $TempRoot "server.stderr.log"
    $ProcessArguments = @(
      "--config", $Metadata.Config,
      "--data", (Join-Path $TempRoot "data"),
      "--port", "$Port",
      "--admin-listen", "127.0.0.1:$AdminPort"
    )
    $StartParameters = @{
      FilePath = $Server
      ArgumentList = $ProcessArguments
      PassThru = $true
      RedirectStandardOutput = $Stdout
      RedirectStandardError = $Stderr
    }
    if ($RunningOnWindows) { $StartParameters.WindowStyle = "Hidden" }
    $Process = Start-Process @StartParameters

    try {
      $Ready = $false
      for ($Attempt = 0; $Attempt -lt 100; $Attempt++) {
        if ($Process.HasExited) {
          throw "Broker exited while starting profile $Name.`n$(Get-Content $Stderr -Raw)"
        }
        try {
          Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$AdminPort/health/ready" -TimeoutSec 1 | Out-Null
          $Ready = $true
          break
        } catch {
          Start-Sleep -Milliseconds 100
        }
      }
      if (-not $Ready) {
        throw "Broker did not become ready for profile $Name."
      }

      $Metrics = (Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$AdminPort/metrics").Content
      if ($Metrics -notmatch "boltstream_runtime_io_workers $($Metadata.Io)(\r?\n)") {
        throw "Profile $Name did not expose io_workers=$($Metadata.Io)."
      }
      if ($Metrics -notmatch "boltstream_runtime_append_workers $($Metadata.Append)(\r?\n)") {
        throw "Profile $Name did not expose append_workers=$($Metadata.Append)."
      }

      foreach ($Workload in @("produce-throughput", "produce-latency", "fetch-throughput")) {
        $WorkloadMessages = if ($Workload -eq "fetch-throughput") { $FetchMessages } else { $Messages }
        $Json = Join-Path $OutputDir "$Name-$Workload.json"
        $Markdown = Join-Path $OutputDir "$Name-$Workload.md"
        $Arguments = @(
          "run", "--workload", $Workload,
          "--host", "127.0.0.1", "--port", "$Port", "--admin-port", "$AdminPort",
          "--profile", $Name, "--environment", $EnvironmentKind, "--machine-type", $MachineType,
          "--partitions", "4", "--clients", "$Clients",
          "--duration-seconds", "$Duration", "--warmup-seconds", "$WarmupSeconds",
          "--messages", "$WorkloadMessages", "--warmup-messages", "$WarmupMessages",
          "--payload-bytes", "256", "--key-bytes", "16", "--repetitions", "$Repetitions",
          "--server-io-workers", "$($Metadata.Io)",
          "--server-append-workers", "$($Metadata.Append)",
          "--server-append-batch-records", "$($Metadata.Batch)",
          "--server-queue-depth", "1024", "--server-log-level", "warn",
          "--json-out", $Json, "--markdown-out", $Markdown
        )
        & $Bench @Arguments | Out-Null
        if ($LASTEXITCODE -ne 0) {
          throw "Benchmark workload $Workload failed for profile $Name with exit $LASTEXITCODE."
        }
        $Result = Get-Content $Json -Raw | ConvertFrom-Json
        if ($Result.summary.errors -ne 0) {
          throw "Benchmark workload $Workload reported errors for profile $Name."
        }
        if ($Workload -eq "produce-throughput") {
          $Batches = [double](($Result.repetitions | Measure-Object -Property append_batches -Sum).Sum)
          $Records = [double](($Result.repetitions | Measure-Object -Property append_batch_records -Sum).Sum)
          if ($Name -eq "batched-writes" -and ($Batches -le 0 -or $Records / $Batches -le 1.0)) {
            throw "Batched profile did not prove an average append batch size above one."
          }
          if ($Name -ne "batched-writes" -and $Batches -ne $Records) {
            throw "Unbatched profile $Name emitted a batch larger than one."
          }
        }
      }
      Write-Host "Phase 10 benchmark profile passed: $Name"
    } finally {
      if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
      }
      Remove-Item -Recurse -Force -LiteralPath $TempRoot -ErrorAction SilentlyContinue
    }
  }
} finally {
  if ($HadToken) {
    $env:BOLTSTREAM_BROKER_TOKEN = $PreviousToken
  } else {
    Remove-Item Env:\BOLTSTREAM_BROKER_TOKEN -ErrorAction SilentlyContinue
  }
}

Write-Host "Phase 10 benchmark matrix passed. Results: $OutputDir"
