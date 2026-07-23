# Host-side unit checks (no NCS / Zephyr required).
# Run from repo root or this directory:
#   powershell -File tests/host/run.ps1

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$cc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $cc) {
    $cc = Get-Command clang -ErrorAction SilentlyContinue
}
if (-not $cc) {
    Write-Error "Need gcc or clang on PATH to build host tests"
}

& $cc.Source -O0 -Wall -Wextra -o test_security_timeout.exe test_security_timeout.c
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\test_security_timeout.exe
exit $LASTEXITCODE
