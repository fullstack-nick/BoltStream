param(
  [Parameter(Mandatory = $true)][string]$InputDir,
  [string]$CanonicalJson = "benchmarks/results/phase-10-gcp-e2-micro.json",
  [string]$Markdown = "docs/benchmarks.md",
  [string]$Readme = "README.md",
  [string]$RequiredEnvironment = "gcp-e2-micro",
  [switch]$Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RequiredProfiles = @("single-threaded", "worker-event-loops", "batched-writes")
$RequiredWorkloads = @("produce-throughput", "produce-latency", "fetch-throughput")
$ExpectedProfiles = @{
  "single-threaded" = @{ Io = 1; Append = 0; Batch = 1 }
  "worker-event-loops" = @{ Io = 2; Append = 2; Batch = 1 }
  "batched-writes" = @{ Io = 2; Append = 2; Batch = 32 }
}
$Files = @(Get-ChildItem -Path $InputDir -Filter *.json -File | Sort-Object Name)
if ($Files.Count -eq 0) {
  throw "No benchmark JSON files found in $InputDir."
}

function Get-Median([double[]]$Values) {
  if ($Values.Count -eq 0) { return 0.0 }
  $Sorted = @($Values | Sort-Object)
  $Middle = [math]::Floor($Sorted.Count / 2)
  if ($Sorted.Count % 2 -eq 0) { return ($Sorted[$Middle - 1] + $Sorted[$Middle]) / 2.0 }
  return $Sorted[$Middle]
}

function Get-Cv([double[]]$Values) {
  if ($Values.Count -lt 2) { return 0.0 }
  $Mean = ($Values | Measure-Object -Average).Average
  if ($Mean -eq 0) { return 0.0 }
  $Squares = 0.0
  foreach ($Value in $Values) { $Squares += [math]::Pow($Value - $Mean, 2) }
  return [math]::Sqrt($Squares / ($Values.Count - 1)) / $Mean * 100.0
}

$Documents = @()
foreach ($File in $Files) {
  $RoundMatch = [regex]::Match($File.BaseName, '-r(?<round>[1-9][0-9]*)$')
  if (-not $RoundMatch.Success) {
    throw "$($File.Name) does not identify its benchmark round."
  }
  $Raw = Get-Content $File.FullName -Raw
  if ($Raw -match '(?i)token|secret|(?:\d{1,3}\.){3}\d{1,3}') {
    throw "Publication rejected potentially sensitive content in $($File.Name)."
  }
  $Document = $Raw | ConvertFrom-Json
  if ($Document.schema_version -ne 1) { throw "$($File.Name) has an unsupported schema." }
  if ($Document.environment.kind -ne $RequiredEnvironment) {
    throw "$($File.Name) environment '$($Document.environment.kind)' is not '$RequiredEnvironment'."
  }
  if ($Document.environment.build_type -ne "Release") {
    throw "$($File.Name) is not a Release benchmark."
  }
  if ($Document.summary.errors -ne 0) { throw "$($File.Name) contains benchmark errors." }
  if ($Document.environment.protocol_version -ne "4" -or
      $Document.environment.storage_format_version -ne "2") {
    throw "$($File.Name) does not use protocol v4 and storage format v2."
  }
  if ($Document.environment.git_sha -notmatch '^[0-9a-f]{12,40}$') {
    throw "$($File.Name) has an invalid Git SHA."
  }
  if (-not $ExpectedProfiles.ContainsKey($Document.broker.profile)) {
    throw "$($File.Name) has an unknown benchmark profile."
  }
  $Expected = $ExpectedProfiles[$Document.broker.profile]
  if ($Document.broker.io_workers -ne $Expected.Io -or
      $Document.broker.append_workers -ne $Expected.Append -or
      $Document.broker.append_batch_records -ne $Expected.Batch -or
      $Document.broker.append_queue_depth -ne 1024 -or
      $Document.broker.durability -ne "flush" -or
      -not $Document.broker.metrics_enabled -or
      $Document.broker.log_level -ne "warn") {
    throw "$($File.Name) does not match its checked-in broker profile."
  }
  if ($Document.workload.partitions -ne 4 -or $Document.workload.clients -ne 16 -or
      $Document.workload.payload_bytes -ne 256 -or $Document.workload.key_bytes -ne 16) {
    throw "$($File.Name) does not match the headline topology and payload contract."
  }
  switch ($Document.workload.name) {
    "produce-throughput" {
      if ($Document.workload.duration_seconds -ne 30 -or
          $Document.workload.warmup_seconds -ne 60) {
        throw "$($File.Name) does not match the produce-throughput timing contract."
      }
    }
    "produce-latency" {
      if ($Document.workload.messages -ne 100000 -or
          $Document.workload.warmup_messages -ne 10000) {
        throw "$($File.Name) does not match the produce-latency message contract."
      }
    }
    "fetch-throughput" {
      if ($Document.workload.messages -ne 250000 -or
          $Document.workload.preload_method -ne "direct-batched-storage-setup" -or
          $Document.workload.preload_batch_records -ne 1024) {
        throw "$($File.Name) does not match the fetch preload and count contract."
      }
    }
    default { throw "$($File.Name) has an unknown workload." }
  }
  $Document | Add-Member -NotePropertyName _round -NotePropertyValue ([int]$RoundMatch.Groups['round'].Value)
  $Document | Add-Member -NotePropertyName _source_file -NotePropertyValue $File.Name
  $Documents += $Document
}

$GitShas = @($Documents | ForEach-Object { $_.environment.git_sha } | Sort-Object -Unique)
if ($GitShas.Count -ne 1 -or [string]::IsNullOrWhiteSpace($GitShas[0])) {
  throw "All benchmark inputs must use one non-empty Git SHA."
}
$Environment = $Documents[0].environment
$GeneratedAt = @($Documents | ForEach-Object { $_.generated_at_utc } | Sort-Object)[-1]

$Results = @()
foreach ($Profile in $RequiredProfiles) {
  $ProfileDocuments = @($Documents | Where-Object { $_.broker.profile -eq $Profile })
  if ($ProfileDocuments.Count -eq 0) { throw "Missing benchmark profile $Profile." }
  $Workloads = @()
  foreach ($Workload in $RequiredWorkloads) {
    $Matches = @($ProfileDocuments | Where-Object { $_.workload.name -eq $Workload })
    if ($Matches.Count -eq 0) { throw "Missing workload $Workload for profile $Profile." }
    $RoundNumbers = @($Matches | ForEach-Object { $_._round } | Sort-Object -Unique)
    if ($RoundNumbers.Count -ne $Matches.Count) {
      throw "$Profile/$Workload contains duplicate benchmark rounds."
    }
    $Repetitions = @(
      foreach ($Match in ($Matches | Sort-Object _round)) {
        $InputRepetitions = @($Match.repetitions)
        if ($InputRepetitions.Count -ne 1) {
          throw "$($Match._source_file) must contain exactly one live repetition."
        }
        $InputRepetition = $InputRepetitions[0]
        if ($InputRepetition.errors -ne 0 -or $InputRepetition.records -le 0) {
          throw "$($Match._source_file) contains an invalid measured repetition."
        }
        if ($Workload -eq "produce-latency" -and $InputRepetition.records -ne 100000) {
          throw "$($Match._source_file) did not acknowledge exactly 100,000 measured records."
        }
        if ($Workload -eq "fetch-throughput" -and $InputRepetition.records -ne 250000) {
          throw "$($Match._source_file) did not verify exactly 250,000 fetched records."
        }
        if ($Workload -eq "fetch-throughput" -and
            ($InputRepetition.append_batches -ne 0 -or $InputRepetition.append_batch_records -ne 0)) {
          throw "$($Match._source_file) included untimed preload appends in its broker metric deltas."
        }
        if ($Workload -like "produce-*" -and $Profile -eq "batched-writes" -and
            ($InputRepetition.append_batches -le 0 -or
             $InputRepetition.append_batch_records / $InputRepetition.append_batches -le 1.0)) {
          throw "$($Match._source_file) did not prove internal append batching."
        }
        if ($Workload -like "produce-*" -and $Profile -ne "batched-writes" -and
            $InputRepetition.append_batches -ne $InputRepetition.append_batch_records) {
          throw "$($Match._source_file) emitted a batch larger than one for an unbatched profile."
        }
        [pscustomobject][ordered]@{
          round = $Match._round
          generated_at_utc = $Match.generated_at_utc
          records = $InputRepetition.records
          bytes = $InputRepetition.bytes
          seconds = $InputRepetition.seconds
          records_per_second = $InputRepetition.records_per_second
          mebibytes_per_second = $InputRepetition.mebibytes_per_second
          errors = $InputRepetition.errors
          latency_us = $InputRepetition.latency_us
          append_batches = $InputRepetition.append_batches
          append_batch_records = $InputRepetition.append_batch_records
        }
      }
    )
    if ($Repetitions.Count -lt 2) {
      throw "$Profile/$Workload has $($Repetitions.Count) repetitions; two completed rounds are required."
    }
    $Throughput = [double[]]@($Repetitions | ForEach-Object { $_.records_per_second })
    $Mebibytes = [double[]]@($Repetitions | ForEach-Object { $_.mebibytes_per_second })
    $P50 = [double[]]@($Repetitions | ForEach-Object { $_.latency_us.p50 })
    $P95 = [double[]]@($Repetitions | ForEach-Object { $_.latency_us.p95 })
    $P99 = [double[]]@($Repetitions | ForEach-Object { $_.latency_us.p99 })
    $Max = [double[]]@($Repetitions | ForEach-Object { $_.latency_us.max })
    $ThroughputCv = Get-Cv $Throughput
    $Unstable = $ThroughputCv -gt 15.0
    $Summary = [ordered]@{
      records_per_second = [ordered]@{
        median = Get-Median $Throughput
        min = ($Throughput | Measure-Object -Minimum).Minimum
        max = ($Throughput | Measure-Object -Maximum).Maximum
        cv_percent = $ThroughputCv
      }
      mebibytes_per_second = [ordered]@{
        median = Get-Median $Mebibytes
        min = ($Mebibytes | Measure-Object -Minimum).Minimum
        max = ($Mebibytes | Measure-Object -Maximum).Maximum
        cv_percent = Get-Cv $Mebibytes
      }
      latency_us = [ordered]@{
        p50 = Get-Median $P50
        p95 = Get-Median $P95
        p99 = Get-Median $P99
        max = ($Max | Measure-Object -Maximum).Maximum
      }
      record_count = ($Repetitions | Measure-Object -Property records -Sum).Sum
      error_count = ($Repetitions | Measure-Object -Property errors -Sum).Sum
      append_batches = ($Repetitions | Measure-Object -Property append_batches -Sum).Sum
      append_batch_records = ($Repetitions | Measure-Object -Property append_batch_records -Sum).Sum
      instability_warning = $Unstable
    }
    $Workloads += [ordered]@{
      name = $Workload
      configuration = $Matches[0].workload
      repetitions = $Repetitions
      summary = $Summary
    }
  }
  $Results += [ordered]@{
    profile = $Profile
    broker = $ProfileDocuments[0].broker
    workloads = $Workloads
  }
}

$RecommendedProfile = ($Results | Sort-Object {
  (($_.workloads | Where-Object { $_.name -eq 'produce-throughput' }).summary.records_per_second.median)
} -Descending | Select-Object -First 1).profile
$AllRounds = @($Documents | ForEach-Object { $_._round } | Sort-Object -Unique)
$RoundOrders = @()
foreach ($Round in $AllRounds) {
  $Shift = ($Round - 1) % $RequiredProfiles.Count
  $Order = if ($Shift -eq 0) {
    $RequiredProfiles
  } else {
    @($RequiredProfiles[$Shift..($RequiredProfiles.Count - 1)] + $RequiredProfiles[0..($Shift - 1)])
  }
  $RoundOrders += [ordered]@{ round = $Round; profiles = $Order }
}

$Canonical = [ordered]@{
  schema_version = 1
  generated_at_utc = $GeneratedAt
  source_git_sha = $GitShas[0]
  environment = $Environment
  methodology = [ordered]@{
    primary_environment = "GCP Ubuntu e2-micro"
    transport = "co-located loopback client and broker"
    durability = "flush"
    profiles = $RequiredProfiles
    profile_order = "rotated across rounds"
    headline = "produce throughput and acknowledged produce latency"
    recommended_profile = $RecommendedProfile
    profile_order_by_round = $RoundOrders
    campaign_plan = "five rotated rounds"
    campaign_completion = "two complete rounds for every profile plus round three for single-threaded and batched-writes"
    sample_policy = "all completed profile triplets retained; interrupted worker-event-loops round three excluded"
    limited_sample = $true
    targets_are_release_gates = $false
  }
  results = $Results
}
$CanonicalText = (($Canonical | ConvertTo-Json -Depth 20).Replace("`r`n", "`n")) + "`n"

function Find-Workload($ProfileResult, [string]$Name) {
  return $ProfileResult.workloads | Where-Object { $_.name -eq $Name } | Select-Object -First 1
}

$MarkdownLines = @(
  "# BoltStream Benchmarks",
  "",
  "Canonical commit: ``$($GitShas[0])``",
  "",
  "Environment: GCP ``$($Environment.machine_type)``; $($Environment.os); $($Environment.cpu_model); $($Environment.logical_cpus) logical CPUs; $($Environment.memory_bytes) bytes RAM; $($Environment.compiler); Release build.",
  "",
  "The client and broker run together on the VM over loopback. The e2-micro is shared-core and may burst, so these numbers describe this exact environment rather than dedicated-host capacity. Every profile uses protocol v4, storage format v2, 256-byte payloads, 16-byte keys, four partitions, metrics enabled, warning-level logs, and flush durability.",
  "",
  "**Limited campaign:** the operator stopped the planned five-round run after two complete rotated rounds for every profile and a third complete round for single-threaded and batched-writes. Every completed profile triplet is retained. The interrupted worker-event-loops round three produced no complete local triplet and is excluded. These results are sufficient for bounded Phase 10 engineering evidence but are not a five-round capacity study.",
  "",
  "The measured recommendation is **$RecommendedProfile**. It is an explicit benchmark profile; ordinary compiled, Compose, and GCP defaults remain compatibility-oriented.",
  "",
  "## Headline Produce Results",
  "",
  "| Profile | Median records/s | Min | Max | CV | Median MiB/s | p50 (us) | p95 (us) | p99 (us) | max (us) |",
  "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
)
foreach ($Result in $Results) {
  $Throughput = Find-Workload $Result "produce-throughput"
  $Latency = Find-Workload $Result "produce-latency"
  $MarkdownLines += "| $($Result.profile) | $([math]::Round($Throughput.summary.records_per_second.median, 0)) | $([math]::Round($Throughput.summary.records_per_second.min, 0)) | $([math]::Round($Throughput.summary.records_per_second.max, 0)) | $([math]::Round($Throughput.summary.records_per_second.cv_percent, 2))% | $([math]::Round($Throughput.summary.mebibytes_per_second.median, 3)) | $([math]::Round($Latency.summary.latency_us.p50, 3)) | $([math]::Round($Latency.summary.latency_us.p95, 3)) | $([math]::Round($Latency.summary.latency_us.p99, 3)) | $([math]::Round($Latency.summary.latency_us.max, 3)) |"
}
$MarkdownLines += @(
  "",
  "## Fetch Results",
  "",
  "| Profile | Median records/s | Min | Max | CV | Median MiB/s |",
  "| --- | ---: | ---: | ---: | ---: | ---: |"
)
foreach ($Result in $Results) {
  $Fetch = Find-Workload $Result "fetch-throughput"
  $MarkdownLines += "| $($Result.profile) | $([math]::Round($Fetch.summary.records_per_second.median, 0)) | $([math]::Round($Fetch.summary.records_per_second.min, 0)) | $([math]::Round($Fetch.summary.records_per_second.max, 0)) | $([math]::Round($Fetch.summary.records_per_second.cv_percent, 2))% | $([math]::Round($Fetch.summary.mebibytes_per_second.median, 3)) |"
}
$Warnings = @()
foreach ($Result in $Results) {
  foreach ($Workload in $Result.workloads) {
    if ($Workload.summary.instability_warning) {
      $Warnings += "$($Result.profile)/$($Workload.name) exceeded 15% throughput CV across $($Workload.repetitions.Count) completed rounds; the limited campaign ended without the planned variance-extension rounds."
    }
  }
}
if ($Warnings.Count -gt 0) {
  $MarkdownLines += @("", "## Instability Warning", "")
  foreach ($Warning in $Warnings) { $MarkdownLines += "- $Warning" }
}
$MarkdownLines += @("", "## Every Measured Round", "",
  "| Profile | Workload | Round | Records/s | MiB/s | Records | Errors | Append batches | Batch records |",
  "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
foreach ($Result in $Results) {
  foreach ($Workload in $Result.workloads) {
    foreach ($Repetition in $Workload.repetitions) {
      $MarkdownLines += "| $($Result.profile) | $($Workload.name) | $($Repetition.round) | $([math]::Round($Repetition.records_per_second, 3)) | $([math]::Round($Repetition.mebibytes_per_second, 3)) | $($Repetition.records) | $($Repetition.errors) | $($Repetition.append_batches) | $($Repetition.append_batch_records) |"
    }
  }
}
$MarkdownLines += @(
  "",
  "Fetch setup is excluded from the timed result: each disposable topic is created through the authenticated broker, deterministically preloaded while the isolated broker is stopped using storage batches of 1,024, and then read and fully verified through four authenticated partition-specific consumers after the exact target profile restarts.",
  "",
  "The original 100k/400k/750k targets remain aspirational developer-class goals, not Phase 10 completion gates. Full per-repetition results, batch counters, workload parameters, and dispersion data are stored in [the canonical JSON](../benchmarks/results/phase-10-gcp-e2-micro.json). Native Windows Release measurements are documented [separately](benchmarks-windows.md).",
  ""
)
$MarkdownText = ($MarkdownLines -join "`n")

$HeadlineStart = "<!-- PHASE10_BENCHMARK_START -->"
$HeadlineEnd = "<!-- PHASE10_BENCHMARK_END -->"
$ReadmeText = Get-Content $Readme -Raw
$StartIndex = $ReadmeText.IndexOf($HeadlineStart)
$EndIndex = $ReadmeText.IndexOf($HeadlineEnd)
if ($StartIndex -lt 0 -or $EndIndex -le $StartIndex) {
  throw "README benchmark publication markers are missing."
}
$TableStart = $MarkdownLines.IndexOf("| Profile | Median records/s | Min | Max | CV | Median MiB/s | p50 (us) | p95 (us) | p99 (us) | max (us) |")
$TableEnd = $TableStart + 1 + $RequiredProfiles.Count
$ReadmeBlock = @($HeadlineStart, "", "Limited GCP ``e2-micro`` results for exact commit ``$($GitShas[0])`` (two complete rounds for all profiles; a third for single-threaded and batched-writes). Measured recommendation: **$RecommendedProfile**.", "")
$ReadmeBlock += $MarkdownLines[$TableStart..$TableEnd]
$ReadmeBlock += @("", "See [docs/benchmarks.md](docs/benchmarks.md) for methodology, fetch results, dispersion, and the canonical JSON.", "", $HeadlineEnd)
$NewReadme = $ReadmeText.Substring(0, $StartIndex) + ($ReadmeBlock -join "`n") + $ReadmeText.Substring($EndIndex + $HeadlineEnd.Length)

if ($Check) {
  if (-not (Test-Path $CanonicalJson) -or (Get-Content $CanonicalJson -Raw) -ne $CanonicalText) {
    throw "Canonical benchmark JSON is not regenerated exactly."
  }
  if (-not (Test-Path $Markdown) -or (Get-Content $Markdown -Raw) -ne $MarkdownText) {
    throw "Benchmark Markdown is not regenerated exactly."
  }
  if ($ReadmeText -ne $NewReadme) {
    throw "README benchmark table is not regenerated exactly."
  }
  Write-Host "Phase 10 benchmark publication is reproducible."
  exit 0
}

New-Item -ItemType Directory -Force -Path (Split-Path $CanonicalJson), (Split-Path $Markdown) | Out-Null
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $CanonicalJson), $CanonicalText, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $Markdown), $MarkdownText, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path (Get-Location) $Readme), $NewReadme, [System.Text.UTF8Encoding]::new($false))
Write-Host "Published canonical Phase 10 benchmark results for $($GitShas[0])."
