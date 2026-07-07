param(
  [string]$Preset = "windows-gcc-debug",
  [string]$GitSha = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($GitSha)) {
  $GitSha = (git rev-parse --short=12 HEAD 2>$null)
  if ([string]::IsNullOrWhiteSpace($GitSha)) {
    $GitSha = "unknown"
  }
}

& "$PSScriptRoot\build.ps1" -Preset $Preset

$buildDir = Join-Path (Get-Location) "build\$Preset"
$distDir = Join-Path (Get-Location) "dist\boltstream-$GitSha"
$artifactDir = Join-Path (Get-Location) "artifacts"
Remove-Item -Recurse -Force -LiteralPath $distDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $distDir, $artifactDir | Out-Null

cmake --install $buildDir --prefix $distDir

$artifact = Join-Path $artifactDir "boltstream-windows-x86_64-$GitSha.zip"
Remove-Item -Force -LiteralPath $artifact -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $distDir "*") -DestinationPath $artifact
Write-Host "Created $artifact"

