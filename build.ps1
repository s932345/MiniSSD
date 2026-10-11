param(
    [ValidateSet('all', 'test', 'demo', 'report')]
    [string]$Action = 'all',
    [string]$Compiler = 'g++',
    [ValidateSet('Release', 'Debug')]
    [string]$BuildType = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$compilerCommand = Get-Command -Name $Compiler -CommandType Application -ErrorAction SilentlyContinue
if ($compilerCommand) {
    $resolvedCompiler = $compilerCommand.Source
} elseif ($Compiler -eq 'g++' -and (Test-Path -LiteralPath 'C:\msys64\ucrt64\bin\g++.exe')) {
    $resolvedCompiler = 'C:\msys64\ucrt64\bin\g++.exe'
} else {
    throw "C++ compiler not found: $Compiler. Use -Compiler with an absolute g++ path."
}

$buildDirectory = Join-Path $PSScriptRoot 'build'
$null = New-Item -ItemType Directory -Path $buildDirectory -Force
$commonArguments = @(
    '-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Wconversion',
    '-Wsign-conversion', '-Werror', '-D_GLIBCXX_ASSERTIONS',
    '-static-libgcc', '-static-libstdc++',
    '-I', 'include', 'src/nand_flash.cpp', 'src/ftl.cpp', 'src/workloads.cpp'
)
if ($BuildType -eq 'Release') { $commonArguments += @('-O2', '-DNDEBUG') }
else { $commonArguments += @('-O0', '-g') }

function Build-Executable([string]$Source, [string]$Name) {
    $executablePath = Join-Path $buildDirectory $Name
    $compileArguments = $commonArguments + @(
        $Source, '-o', (Join-Path 'build' $Name)
    )
    if ($Source -eq 'src/main.cpp') { $compileArguments += '-municode' }
    Write-Host "Building $Name"
    & $resolvedCompiler @compileArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed: $Name (exit $LASTEXITCODE)"
    }
    return $executablePath
}

# MinGW's linker can misread non-ASCII absolute output paths. Let PowerShell
# enter the project directory, then give the compiler ASCII relative paths.
Push-Location -LiteralPath $PSScriptRoot
try {
    if ($Action -eq 'all' -or $Action -eq 'test') {
        $testExecutable = Build-Executable 'tests/test_nand.cpp' 'nand_tests.exe'
        & $testExecutable
        if ($LASTEXITCODE -ne 0) { throw "NAND tests failed (exit $LASTEXITCODE)" }
        $ftlTestExecutable = Build-Executable 'tests/test_ftl.cpp' 'ftl_tests.exe'
        & $ftlTestExecutable
        if ($LASTEXITCODE -ne 0) { throw "FTL tests failed (exit $LASTEXITCODE)" }
        $gcTestExecutable = Build-Executable 'tests/test_gc.cpp' 'gc_tests.exe'
        & $gcTestExecutable
        if ($LASTEXITCODE -ne 0) { throw "GC tests failed (exit $LASTEXITCODE)" }
    }

    if ($Action -eq 'all' -or $Action -eq 'demo' -or $Action -eq 'report') {
        $demoExecutable = Build-Executable 'src/main.cpp' 'minissd.exe'
        if ($Action -eq 'all' -or $Action -eq 'demo') {
            & $demoExecutable demo
            if ($LASTEXITCODE -ne 0) { throw "MVP demo failed (exit $LASTEXITCODE)" }
        }
        if ($Action -eq 'all' -or $Action -eq 'report') {
            & $demoExecutable validate --seed 20261010 --operations 20000 --report reports/validation.json
            if ($LASTEXITCODE -ne 0) { throw "Full validation failed (exit $LASTEXITCODE)" }
            & $demoExecutable benchmark --seed 20261010 --writes 5000 --report reports/benchmark.json
            if ($LASTEXITCODE -ne 0) { throw "Benchmark validation failed (exit $LASTEXITCODE)" }
        }
    }
} finally {
    Pop-Location
}
