# SPDX-License-Identifier: MIT
#
# Regenerates the golden binary fixtures in tests/fixtures/.
#
# Run this only when a change to a binary format is *intended*, and commit the
# regenerated files together with the change and a note in
# docs/development-log.md. A normal test run never rewrites a fixture.
#
#   pwsh tools/generate-fixtures.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $exe = "build\msvc-release\test_golden_binaries.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "building the golden test binary first..."
        & pwsh -NoProfile -File tools/build.ps1 -Filter golden -NoTests:$false | Out-Null
    }
    if (-not (Test-Path $exe)) { throw "$exe was not built" }

    $env:MINITOOL_UPDATE_GOLDEN = "1"
    & $exe --filter=Golden
    Remove-Item Env:\MINITOOL_UPDATE_GOLDEN

    Write-Host ""
    Write-Host "fixtures regenerated in tests/fixtures:" -ForegroundColor Green
    Get-ChildItem "tests\fixtures" | ForEach-Object {
        Write-Host ("  {0,-20} {1,8} bytes" -f $_.Name, $_.Length)
    }
    Write-Host ""
    Write-Host "review the diff before committing: a changed fixture means a changed format."
}
finally {
    Pop-Location
}
