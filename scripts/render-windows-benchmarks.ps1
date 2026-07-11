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
$ExpectedProfiles = @{
  "single-threaded" = @{ Io = 1; Append = 0; Batch = 1 }
  "worker-event-loops" = @{ Io = 2; Append = 2; Batch = 1 }
  "batched-writes" = @{ Io = 2; Append = 2; Batch = 32 }
}
$Files = @(Get-ChildItem -Path $InputDir -Filter *.json -File | Sort-Object Name)
if ($Files.Count -ne 9) { throw "Windows publication requires exactly nine benchmark JSON files." }

function Format-Number([double]$Value, [int]$Decimals) {
  $Format = if ($Decimals -eq 0) { "0" } else { "0." + [string]::new('0', $Decimals) }
  return $Value.ToString($Format, [Globalization.CultureInfo]::InvariantCulture)
}

function Test-IsJsonNumber($Value) {
  return $Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or
    $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or
    $Value -is [int64] -or $Value -is [uint64] -or $Value -is [single] -or
    $Value -is [double] -or $Value -is [decimal]
}

function Test-JsonValueEquivalent($Left, $Right) {
  if ($null -eq $Left -or $null -eq $Right) { return $null -eq $Left -and $null -eq $Right }
  if ((Test-IsJsonNumber $Left) -and (Test-IsJsonNumber $Right)) {
    return [double]$Left -eq [double]$Right
  }
  if ($Left -is [pscustomobject] -and $Right -is [pscustomobject]) {
    $LeftNames = @($Left.PSObject.Properties.Name)
    $RightNames = @($Right.PSObject.Properties.Name)
    if (($LeftNames -join "`n") -cne ($RightNames -join "`n")) { return $false }
    foreach ($Name in $LeftNames) {
      if (-not (Test-JsonValueEquivalent $Left.$Name $Right.$Name)) { return $false }
    }
    return $true
  }
  if ($Left -is [Collections.IEnumerable] -and $Left -isnot [string] -and
      $Right -is [Collections.IEnumerable] -and $Right -isnot [string]) {
    $LeftItems = @($Left)
    $RightItems = @($Right)
    if ($LeftItems.Count -ne $RightItems.Count) { return $false }
    for ($Index = 0; $Index -lt $LeftItems.Count; ++$Index) {
      if (-not (Test-JsonValueEquivalent $LeftItems[$Index] $RightItems[$Index])) { return $false }
    }
    return $true
  }
  return $Left -ceq $Right
}

function Test-JsonEquivalent([string]$Path, [string]$GeneratedJson) {
  if (-not (Test-Path $Path)) { return $false }
  try {
    $Existing = Get-Content $Path -Raw | ConvertFrom-Json
    $Generated = $GeneratedJson | ConvertFrom-Json
    return Test-JsonValueEquivalent $Existing $Generated
  } catch {
    return $false
  }
}

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
  if (-not $ExpectedProfiles.ContainsKey($Document.broker.profile)) {
    throw "$($File.Name) has an unknown Windows benchmark profile."
  }
  $Expected = $ExpectedProfiles[$Document.broker.profile]
  if ($Document.broker.io_workers -ne $Expected.Io -or
      $Document.broker.append_workers -ne $Expected.Append -or
      $Document.broker.append_batch_records -ne $Expected.Batch -or
      $Document.broker.append_queue_depth -ne 1024 -or
      $Document.broker.durability -ne "flush") {
    throw "$($File.Name) does not match its checked-in broker profile."
  }
  if ($Document.workload.partitions -ne 4 -or $Document.workload.clients -ne 16 -or
      $Document.workload.payload_bytes -ne 256 -or $Document.workload.key_bytes -ne 16 -or
      @($Document.repetitions).Count -ne 3) {
    throw "$($File.Name) does not match the Windows topology and repetition contract."
  }
  switch ($Document.workload.name) {
    "produce-throughput" {
      if ($Document.workload.duration_seconds -ne 3 -or $Document.workload.warmup_seconds -ne 2) {
        throw "$($File.Name) does not match the Windows throughput timing contract."
      }
    }
    "produce-latency" {
      if ($Document.workload.messages -ne 2000 -or $Document.workload.warmup_messages -ne 200) {
        throw "$($File.Name) does not match the Windows latency message contract."
      }
    }
    "fetch-throughput" {
      if ($Document.workload.messages -ne 5000 -or
          $Document.workload.preload_method -ne "authenticated-protocol") {
        throw "$($File.Name) does not match the Windows fetch contract."
      }
    }
    default { throw "$($File.Name) has an unknown Windows workload." }
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
$JsonText = ($Consolidated | ConvertTo-Json -Depth 20 -Compress) + "`n"

$Lines = @(
  "# Native Windows Release Benchmarks",
  "",
  "Secondary comparison for exact commit ``$($GitShas[0])`` on $($Environment.cpu_model), $($Environment.logical_cpus) logical CPUs, $($Environment.memory_bytes) bytes RAM, $($Environment.compiler). GCP ``e2-micro`` remains the headline environment.",
  "",
  "These correctness-sized secondary runs use four partitions, 16 clients, 256-byte values, 16-byte keys, and three repetitions. Produce throughput measures three seconds after a two-second warmup; latency uses 200 warmup plus 2,000 measured messages; fetch uses an authenticated protocol preload of 5,000 records. These parameters intentionally differ from the GCP headline workloads and are retained in the JSON.",
  "",
  "| Profile | Produce records/s | Produce CV | p50 (us) | p95 (us) | p99 (us) | Fetch records/s |",
  "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"
)
foreach ($Profile in $Profiles) {
  $Throughput = $Results | Where-Object { $_.profile -eq $Profile -and $_.workload -eq "produce-throughput" }
  $Latency = $Results | Where-Object { $_.profile -eq $Profile -and $_.workload -eq "produce-latency" }
  $Fetch = $Results | Where-Object { $_.profile -eq $Profile -and $_.workload -eq "fetch-throughput" }
  $Lines += "| $Profile | $(Format-Number $Throughput.summary.records_per_second.median 0) | $(Format-Number $Throughput.summary.records_per_second.cv_percent 2)% | $(Format-Number $Latency.summary.latency_us.p50 3) | $(Format-Number $Latency.summary.latency_us.p95 3) | $(Format-Number $Latency.summary.latency_us.p99 3) | $(Format-Number $Fetch.summary.records_per_second.median 0) |"
}
$Lines += @("", "Every repetition and its exact workload configuration is retained in [the Windows JSON](../benchmarks/results/phase-10-windows-release.json).", "")
$MarkdownText = $Lines -join "`n"

if ($Check) {
  if (-not (Test-JsonEquivalent $OutputJson $JsonText)) {
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
