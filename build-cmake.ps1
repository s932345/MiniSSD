param(
    [ValidateSet('Release', 'Debug')]
    [string]$BuildType = 'Release',
    [string]$Compiler = 'g++'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-Application([string]$Name, [string]$Fallback) {
    $command = Get-Command -Name $Name -CommandType Application -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    if ($Fallback -and (Test-Path -LiteralPath $Fallback)) { return (Resolve-Path -LiteralPath $Fallback).Path }
    throw "Tool not found: $Name. See README.md for build requirements."
}

$cmakePath = Find-Application 'cmake' (Join-Path $PSScriptRoot '.tools\cmake-3.31.12-windows-x86_64\bin\cmake.exe')
$ninjaPath = Find-Application 'ninja' (Join-Path $PSScriptRoot '.tools\ninja\ninja.exe')
$compilerFallback = if ($Compiler -eq 'g++') { 'C:\msys64\ucrt64\bin\g++.exe' } else { '' }
$compilerPath = Find-Application $Compiler $compilerFallback
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'

Push-Location -LiteralPath $PSScriptRoot
try {
    & $cmakePath -S . -B build-cmake -G Ninja "-DCMAKE_BUILD_TYPE=$BuildType" "-DCMAKE_CXX_COMPILER=$compilerPath" "-DCMAKE_MAKE_PROGRAM=$ninjaPath"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    & $cmakePath --build build-cmake --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
    & $ctestPath --test-dir build-cmake --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
} finally {
    Pop-Location
}
