param(
  [switch]$Fix
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$roots = @("include", "src", "tools", "tests", "benchmarks")
$files = foreach ($root in $roots) {
  if (Test-Path $root) {
    Get-ChildItem -Path $root -Recurse -Include *.h,*.hpp,*.cpp
  }
}

if (-not $files) {
  Write-Host "No C++ files found."
  exit 0
}

if ($Fix) {
  foreach ($file in $files) {
    clang-format -i $file.FullName
    if ($LASTEXITCODE -ne 0) {
      throw "clang-format failed for $($file.FullName)"
    }
  }
} else {
  foreach ($file in $files) {
    clang-format --dry-run --Werror $file.FullName
    if ($LASTEXITCODE -ne 0) {
      throw "clang-format check failed for $($file.FullName)"
    }
  }
}
