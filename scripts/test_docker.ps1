<#
.SYNOPSIS
Run the native_sim test suites locally, in the same container CI uses.

.DESCRIPTION
native_sim is Linux-only, so the ztest suites cannot run on a Windows box
directly. This runs them in Nordic's pinned toolchain image instead, which is
the same image .github/workflows/ci.yml uses -- so a green run here means the
same thing a green run there does.

The NCS workspace lives in a named docker volume, because `west update` pulls
several GB. The first run is slow (tens of minutes on a cold volume); every run
after it reuses the workspace and takes about as long as the tests themselves.

Requires Docker (Docker Desktop on Windows) and nothing else -- notably not a
local NCS install, since the container carries the toolchain.

.PARAMETER Rev
NCS revision to initialise the workspace at. Defaults to v3.3.1, matching
NCS_REV in ci.yml. Changing it after the first run has no effect unless you
also pass -Fresh, since the volume already holds a workspace.

.PARAMETER Fresh
Delete the cached workspace volume first, forcing a clean `west init`/`update`.
Use after bumping -Rev, or when a workspace has got into a bad state.

.EXAMPLE
.\scripts\test_docker.ps1

.EXAMPLE
.\scripts\test_docker.ps1 -Fresh
#>
[CmdletBinding()]
param(
    [string]$Rev = "v3.3.1",
    [switch]$Fresh
)

$ErrorActionPreference = "Stop"

$image = "ghcr.io/nrfconnect/sdk-nrf-toolchain:$Rev"
$volume = "nrfproxy-ncs-$Rev"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error @"
docker not found on PATH.

Install Docker Desktop, or run the suites another way:
  - the host-only checks need no container:  powershell -File tests/host/run.ps1
  - or let CI run them by pushing a branch
See TESTING.md for what each tier covers.
"@
}

if ($Fresh) {
    Write-Host "==> removing the cached workspace volume $volume"
    # Not an error if it was never created.
    docker volume rm $volume 2>$null | Out-Null
}

Write-Host "==> repo:   $repo"
Write-Host "==> image:  $image"
Write-Host "==> volume: $volume"

# --entrypoint bash: the image sets its own entrypoint, and we want the env it
# publishes through BASH_ENV, which only bash reads. Non-interactive `bash -c`
# does read BASH_ENV; a login shell would not necessarily.
docker run --rm `
    -v "${repo}:/work" `
    -v "${volume}:/ncs" `
    -e "NCS_REV=$Rev" `
    --entrypoint bash `
    $image `
    /work/scripts/docker_test_entry.sh

if ($LASTEXITCODE -ne 0) {
    Write-Error "native_sim suites failed (exit $LASTEXITCODE)"
}

Write-Host "==> done"
