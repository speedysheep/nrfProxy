# Host-side unit checks (no NCS / Zephyr required).
# Run from repo root or this directory:
#   powershell -File tests/host/run.ps1

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "src"
Set-Location $here

$cc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $cc) {
    $cc = Get-Command clang -ErrorAction SilentlyContinue
}
if (-not $cc) {
    Write-Error "Need gcc or clang on PATH to build host tests"
}

function Invoke-HostTest($name, $sources) {
    $exe = "$name.exe"
    & $cc.Source -O0 -Wall -Wextra -o $exe @sources
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & ".\$exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Invoke-HostTest "test_security_timeout" @("test_security_timeout.c")
Invoke-HostTest "test_uart_rx_retry" @(
    "test_uart_rx_retry.c",
    (Join-Path $src "uart_rx_retry.c")
)
Invoke-HostTest "test_drop_stats" @(
    "test_drop_stats.c",
    (Join-Path $src "drop_stats.c")
)

Write-Host "all host tests passed"
