Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Require-Command {
  param([string]$Name)
  $command = Get-Command $Name -ErrorAction SilentlyContinue
  if (-not $command) {
    throw "Required command not found: $Name"
  }
  Write-Host "$Name -> $($command.Source)"
}

Require-Command cmake
Require-Command ninja
Require-Command clang++
Require-Command clang-format
Require-Command clang-tidy
Require-Command clangd
Require-Command lldb
Require-Command g++
Require-Command gdb
Require-Command py
Require-Command zip
Require-Command unzip
Require-Command curl.exe
Require-Command vcvars64.bat

cmake --version | Select-Object -First 1
Write-Host "ninja $(ninja --version)"
clang++ --version | Select-Object -First 1
clang-tidy --version | Select-Object -First 1
g++ --version | Select-Object -First 1
gdb --version | Select-Object -First 1
py -3.11 --version
lldb --version | Select-Object -First 1

$tmp = Join-Path $env:TEMP "boltstream-toolchain-check"
Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$source = Join-Path $tmp "main.cpp"
@"
#include <concepts>
#include <iostream>
#include <span>
#include <vector>

template <std::integral T>
constexpr T sum(std::span<const T> values) {
  T result{};
  for (T value : values) {
    result += value;
  }
  return result;
}

int main() {
  const std::vector<int> values{1, 2, 3, 4};
  std::cout << sum<int>(values) << "\n";
  return 0;
}
"@ | Set-Content -Path $source -Encoding ASCII

& g++ -std=c++20 -Wall -Wextra -pedantic $source -o (Join-Path $tmp "gcc-test.exe")
& clang++ -std=c++20 -Wall -Wextra -pedantic $source -o (Join-Path $tmp "clang-test.exe")

$vcvars = (Get-Command vcvars64.bat).Source
$msvcExe = Join-Path $tmp "msvc-test.exe"
$cmd = "call `"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W4 `"$source`" /Fe:`"$msvcExe`""
cmd.exe /d /s /c $cmd
if ($LASTEXITCODE -ne 0) {
  throw "MSVC C++20 toolchain check failed"
}

Write-Host "Native C++ toolchain check passed."

