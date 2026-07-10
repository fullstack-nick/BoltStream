param(
  [Parameter(Mandatory = $true)][string]$InputDir,
  [string]$OutputJson = "benchmarks/results/phase-10-windows-release.json",
  [string]$OutputMarkdown = "docs/benchmarks-windows.md",
  [switch]$Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Profiles = @("single-threaded", "worker-event-loops", "batched-writes")
$Workloads = @("produce-throughput", "produce-latency", "fetch-throughput")
$Files = @(Get-ChildItem -Path $InputDir -Filter *.json -File | Sort-Object Name)
if ($Files.Count -ne 9) { throw "Windows publication requires exactly nine benchmark JSON files." }

$Documents = @()
foreach ($File in $Files) {
  $Raw = Get-Content $File.FullName -Raw
  if ($Raw -match '(?i)token|secret|(?:\d{1,3}\.){3}\d{1,3}') {
    throw "Publication rejected potentially sensitive content in $($File.Name)."
  }
  $Document = $Raw | ConvertFrom-Json
  if ($Document.schema_version -ne 1 -or $Document.environment.kind -ne "native-windows" -or
      $Document.environment.build_type -ne "Release" -or $Document.summary.errors -ne 0) {
    throw "$($File.Name) is not a valid native Windows Release benchmark."
  }
  if ($Document.environment.protocol_version -ne "4" -or
      $Document.environment.storage_format_version -ne "2") {
    throw "$($File.Name) does not use protocol v4 and storage format v2."
  }
  $Documents += $Document
}

$GitShas = @($Documents | ForEach-Object { $_.environment.git_sha } | Sort-Object -Unique)
if ($GitShas.Count -ne 1 -or $GitShas[0] -notmatch '^[0-9a-f]{12,40}$') {
  throw "Windows inputs must use one valid Git SHA."
}

$Results = @()
foreach ($Profile in $Profiles) {
  foreach ($Workload in $Workloads) {
    $Matches = @($Documents | Where-Object {
      $_.broker.profile -eq $Profile -and $_.workload.name -eq $Workload
    })
    if ($Matches.Count -ne 1) { throw "Missing or duplicate $Profile/$Workload result." }
    if (@($Matches[0].repetitions).Count -lt 1) { throw "$Profile/$Workload has no repetitions." }
    $Results += [ordered]@{
      profile = $Profile
      workload = $Workload
      broker = $Matches[0].broker
      configuration = $Matches[0].workload
      repetitions = $Matches[0].repetitions
      summary = $Matches[0].summary
    }
  }
}

$Environment = $Documents[0].environment
$GeneratedAt = @($Documents | ForEach-Object { $_.generated_at_utc } | Sort-Object)[-1]
$Consolidated = [ordered]@{
  schema_version = 1
  generated_at_utc = $GeneratedAt
  source_git_sha = $GitShas[0]
  environment = $Environment
  methodology = [ordered]@{
    role = "secondary native Windows Release comparison"
    durability = "flush"
    targets_are_release_gates = $false
  }
  results = $Results
}
$JsonText = ($Consolidated | ConvertTo-Json -Depth 20) + "`n"

$Lines = @(
  "# Native Windows Release Benchmarks",
  "",
  "Secondary comparison for exact commit ``$($GitShas[0])`` on $($Environment.cpu_model), $($Environment.logical_cpus) logical CPUs, $($Environment.memory_bytes) bytes RAM, $($Environment.compiler). GCP ``e2-micro`` remains the headline environment.",
  "",
  "| Profile | Produce records/s | Produce CV | p50 (us) | p95 (us) | p99 (us) | Fetch records/s |",
  "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"
)
foreach ($Profile in $Profiles) {
  $Throughput = $Results | Where-Object { $_.profile -eq $Profile -and $_.workload -eq "produce-throughput" }
  $Latency = $Results | Where-Object { $_.profile -eq $Profile -and $_.workload -eq "produce-latency" }
  $Fetch = $Results | Where-Object { $_.profile -eq $Profile -and $_.workload -eq "fetch-throughput" }
  $Lines += "| $Profile | $([math]::Round($Throughput.summary.records_per_second.median, 0)) | $([math]::Round($Throughput.summary.records_per_second.cv_percent, 2))% | $([math]::Round($Latency.summary.latency_us.p50, 3)) | $([math]::Round($Latency.summary.latency_us.p95, 3)) | $([math]::Round($Latency.summary.latency_us.p99, 3)) | $([math]::Round($Fetch.summary.records_per_second.median, 0)) |"
}
$Lines += @("", "Every repetition and its exact workload configuration is retained in [the Windows JSON](../benchmarks/results/phase-10-windows-release.json).", "")
$MarkdownText = $Lines -join "`n"

if ($Check) {
  if (-not (Test-Path $OutputJson) -or (Get-Content $OutputJson -Raw) -ne $JsonText) {
    throw "Windows benchmark JSON is not regenerated exactly."
  }
  if (-not (Test-Path $OutputMarkdown) -or (Get-Content $OutputMarkdown -Raw) -ne $MarkdownText) {
    throw "Windows benchmark Markdown is not regenerated exactly."
  }
  Write-Host "Phase 10 Windows benchmark publication is reproducible."
  exit 0
}

New-Item -ItemType Directory -Force -Path (Split-Path $OutputJson), (Split-Path $OutputMarkdown) | Out-Null
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $OutputJson), $JsonText, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $OutputMarkdown), $MarkdownText, [System.Text.UTF8Encoding]::new($false))
Write-Host "Published secondary Phase 10 Windows results for $($GitShas[0])."
