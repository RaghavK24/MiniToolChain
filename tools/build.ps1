# SPDX-License-Identifier: MIT
#
# Zero-dependency build for machines without CMake: locates the MSVC toolchain
# with vswhere, compiles the library, the `minitool` driver and every test
# binary, then runs the suite. CMake remains the portable build (see README);
# this exists so the project can be built and tested with nothing installed but
# Visual Studio Build Tools.
#
#   pwsh tools/build.ps1                 # build + run all tests
#   pwsh tools/build.ps1 -NoTests        # build only
#   pwsh tools/build.ps1 -Filter lexer   # run only matching test binaries
#   pwsh tools/build.ps1 -Config debug   # unoptimized build with /RTC1
#
# Everything runs with the repository root as the working directory and uses
# relative paths, because the repository path may contain spaces and nested
# quoting through cmd.exe is not worth the risk.

[CmdletBinding()]
param(
    [switch]$NoTests,
    [string]$Filter = "",
    [ValidateSet("debug", "release")][string]$Config = "release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $build = "build\msvc-$Config"
    $objdir = "$build\obj"
    New-Item -ItemType Directory -Force $objdir | Out-Null

    # ------------------------------------------------------------ toolchain --
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
    }
    $vcvars = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath |
        ForEach-Object { Join-Path $_ "VC\Auxiliary\Build\vcvars64.bat" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvars) { throw "vcvars64.bat not found. Install the MSVC C++ build tools." }

    # A tiny batch shim keeps the vcvars environment and cl invocation in one
    # cmd session without quoting the whole command line twice.
    $shim = "$build\cl.bat"
    Set-Content $shim "@echo off`r`ncall `"$vcvars`" >nul 2>&1`r`ncl %*" -Encoding ascii

    $flags = "/nologo /std:c++latest /EHsc /permissive- /Zc:preprocessor /utf-8 " +
             "/W4 /WX /D_CRT_SECURE_NO_WARNINGS /Iinclude"
    if ($Config -eq "release") { $flags += " /O2 /DNDEBUG" } else { $flags += " /Od /Zi /RTC1" }

    function Invoke-Cl([string]$argline) {
        $output = & cmd /c "$shim $argline 2>&1"
        $failed = $LASTEXITCODE -ne 0
        $output | Where-Object {
            $_ -and $_ -notmatch '^\s*\S+\.cpp\s*$' -and $_ -notmatch '^Generating Code'
        } | ForEach-Object { Write-Host $_ }
        if ($failed) { throw "compilation failed" }
    }

    # -------------------------------------------------------------- library --
    $libSources = Get-ChildItem "src" -Recurse -Filter *.cpp |
        Where-Object { $_.FullName -notlike "*\cli\*" } |
        ForEach-Object { Resolve-Path -Relative $_.FullName }
    Write-Host "==> compiling minitool_core ($($libSources.Count) files, $Config)"
    Invoke-Cl "$flags /c /Fo$objdir\ $($libSources -join ' ')"
    $objs = (Get-ChildItem $objdir -Filter *.obj |
        ForEach-Object { Resolve-Path -Relative $_.FullName }) -join ' '

    # --------------------------------------------------------------- driver --
    Write-Host "==> linking minitool.exe"
    # Each executable gets its own object directory so that the library object
    # list stays exactly the library.
    $clidir = "$build\obj-cli"
    New-Item -ItemType Directory -Force $clidir | Out-Null
    Invoke-Cl "$flags /Fe$build\minitool.exe /Fo$clidir\ src/cli/main.cpp $objs"

    # ---------------------------------------------------------- benchmarks --
    $benchSources = Get-ChildItem "benchmarks" -Filter *.cpp -ErrorAction SilentlyContinue |
        ForEach-Object { Resolve-Path -Relative $_.FullName }
    foreach ($bench in $benchSources) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($bench)
        Write-Host "==> $name"
        $benchobj = "$build\obj-$name"
        New-Item -ItemType Directory -Force $benchobj | Out-Null
        Invoke-Cl "$flags /Fe$build\$name.exe /Fo$benchobj\ $bench $objs"
    }

    if ($NoTests) {
        Write-Host "build complete: $build\minitool.exe"
        exit 0
    }

    # ---------------------------------------------------------------- tests --
    $testSources = Get-ChildItem "tests" -Recurse -Filter *.cpp |
        Where-Object { $_.Name -ne "test_main.cpp" } |
        ForEach-Object { Resolve-Path -Relative $_.FullName }
    $failed = @()
    $passed = 0
    foreach ($test in $testSources) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($test)
        if ($Filter -and $name -notmatch $Filter) { continue }
        Write-Host "==> $name"
        $testobj = "$build\obj-test-$name"
        New-Item -ItemType Directory -Force $testobj | Out-Null
        Invoke-Cl ("$flags /Fe$build\$name.exe /Fo$testobj\ /Itests " +
                   "$test tests/support/test_main.cpp $objs")
        # Merge stderr inside cmd rather than with a PowerShell 2>&1. Windows
        # PowerShell 5.1 turns a native command's stderr into an ErrorRecord,
        # which "Stop" treats as fatal and which prints whatever the filter
        # below decides to drop -- so the same run would both fail and look
        # different under `powershell` and `pwsh`. $LASTEXITCODE comes from
        # cmd, which passes the test binary's own exit code straight through.
        cmd /c "$build\$name.exe 2>&1" | ForEach-Object {
            if ($_ -match '^\[  FAILED|failure|uncaught') { Write-Host $_ -ForegroundColor Red }
            elseif ($_ -match '^\[==========\]') { Write-Host "    $_" }
        }
        if ($LASTEXITCODE -ne 0) { $failed += $name } else { $passed++ }
    }

    Write-Host ""
    if ($failed.Count -gt 0) {
        Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
        exit 1
    }
    Write-Host "all $passed test binaries passed" -ForegroundColor Green
}
finally {
    Pop-Location
}
