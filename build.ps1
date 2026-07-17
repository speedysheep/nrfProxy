<#
.SYNOPSIS
  Recreate the nRF Connect build configurations for nrfProxy (Windows).

.DESCRIPTION
  The build*/ directories are generated output (gitignored). This script is the
  committed, machine-portable definition of every build configuration: board
  target, build dir, and the per-board flags that are easy to forget
  (-DSB_CONFIG_PARTITION_MANAGER=n for DTS partitioning, EXTRA_CONF_FILE=prod.conf
  for production). Running a target reproduces its build*/ dir, which the IDE picks up.

  All boards disable the (deprecated) nRF Connect SDK Partition Manager via
  -DSB_CONFIG_PARTITION_MANAGER=n, so the flash layout comes from the board's
  devicetree partitions (DTS partitioning). sysbuild itself is kept (west flash, the
  nrfProxy/ image subdir); note that with a single image and PM off there is no
  merged.hex. See CLAUDE.md ("DTS partitioning") for the full rationale and the
  resulting per-board flash offsets.

.PARAMETER Toolchain
  Path to the NCS toolchain dir (the one named like '936afb6332'). Falls back to
  the NRFPROXY_TOOLCHAIN env var, then a sensible default.

.PARAMETER Ncs
  Path to the NCS workspace (the 'vX.Y.Z' dir that contains the zephyr/ + nrf/
  modules). Falls back to the NRFPROXY_NCS env var, then a default.

.PARAMETER Proj
  Path to this project. Falls back to the NRFPROXY_PROJ env var, then to the
  directory this script lives in (so it's portable when run from the repo).

.EXAMPLE
  .\build.ps1                 # build every configuration
  .\build.ps1 promicro        # build only the Pro Micro (nice!nano) UF2 config
  .\build.ps1 xiao_prod       # production XIAO (no logging/USB console)

.EXAMPLE
  # Override toolchain / SDK / project locations explicitly:
  .\build.ps1 dk -Toolchain D:\ncs\toolchains\936afb6332 -Ncs D:\ncs\v3.3.1

.EXAMPLE
  # ...or via environment variables (parameters take precedence over these):
  $env:NRFPROXY_NCS = 'D:\ncs\v3.3.1'; .\build.ps1
#>
param(
  [ValidateSet('dk','xiao','xiao_prod','promicro','promicro_prod','dongle','all')]
  [string]$Target = 'all',
  [string]$Toolchain,
  [string]$Ncs,
  [string]$Proj
)

$ErrorActionPreference = 'Stop'

# Resolve a setting from (1) the passed parameter, (2) an environment variable,
# (3) a fallback default — in that order.
function Resolve-Setting([string]$ParamValue, [string]$EnvName, [string]$Default) {
  if ($ParamValue) { return $ParamValue }
  $envVal = [Environment]::GetEnvironmentVariable($EnvName)
  if ($envVal)     { return $envVal }
  return $Default
}

# Project defaults to this script's own directory, so the repo is relocatable.
$proj = Resolve-Setting $Proj      'NRFPROXY_PROJ'      $PSScriptRoot
$ncs  = Resolve-Setting $Ncs       'NRFPROXY_NCS'       'C:\ncs\v3.3.1'
$tc   = Resolve-Setting $Toolchain 'NRFPROXY_TOOLCHAIN' 'C:\ncs\toolchains\936afb6332'

# --- toolchain / SDK environment (NCS v3.3.1, toolchain 936afb6332) -----------
$env:PATH = "$tc;$tc\mingw64\bin;$tc\bin;$tc\opt\bin;$tc\opt\bin\Scripts;$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$tc\opt\zephyr-sdk\riscv64-zephyr-elf\bin;$env:PATH"
$env:PYTHONPATH = "$tc\opt\bin;$tc\opt\bin\Lib;$tc\opt\bin\Lib\site-packages"
$env:NRFUTIL_HOME = "$tc\nrfutil\home"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
# How west finds the workspace (and the 'nrf' module) without us having to cd
# into it -- which matters when the SDK and the project are on different drives:
# west build calls os.path.relpath(source_dir) against the cwd, and that raises
# "path is on mount 'C:', start on mount 'D:'" on Windows. So we run from $proj
# and let ZEPHYR_BASE point at the workspace. See CLAUDE.md ("SDK on another
# drive").
$env:ZEPHYR_BASE = "$ncs\zephyr"

# --- build configuration table -----------------------------------------------
# key => @{ board; dir; cmake (args after --) }
# Source dir must precede '--'; everything after '--' goes to sysbuild/CMake.
# Every config passes -DSB_CONFIG_PARTITION_MANAGER=n to disable the deprecated
# Partition Manager and link from the devicetree code_partition (DTS partitioning),
# while keeping sysbuild itself.
$pmOff = '-DSB_CONFIG_PARTITION_MANAGER=n'
$configs = [ordered]@{
  dk            = @{ board = 'nrf52840dk/nrf52840';            dir = 'build_devkit';       cmake = @($pmOff) }
  xiao          = @{ board = 'xiao_ble/nrf52840';              dir = 'build_xiao';         cmake = @($pmOff) }
  xiao_prod     = @{ board = 'xiao_ble/nrf52840';              dir = 'build_xiao_prod';    cmake = @($pmOff, '-DEXTRA_CONF_FILE=prod.conf') }
  promicro      = @{ board = 'promicro_nrf52840/nrf52840/uf2'; dir = 'build_promicro';      cmake = @($pmOff) }
  promicro_prod = @{ board = 'promicro_nrf52840/nrf52840/uf2'; dir = 'build_promicro_prod'; cmake = @($pmOff, '-DEXTRA_CONF_FILE=prod.conf') }
  dongle        = @{ board = 'nrf52840dongle/nrf52840';        dir = 'build_dongle';       cmake = @($pmOff) }
}

function Invoke-BuildConfig([string]$name) {
  $c = $configs[$name]
  $buildDir = Join-Path $proj $c.dir
  Write-Host "==> Building '$name' : $($c.board) -> $($c.dir)" -ForegroundColor Cyan

  # Run from $proj, not the workspace: ZEPHYR_BASE (set above) is what puts the
  # 'nrf' module in scope, and keeping the cwd on the project's own drive is what
  # lets the SDK live on another one.
  # Source dir ($proj) before '--'; sysbuild/CMake args after it.
  $westArgs = @('build','-b',$c.board,'--pristine','-d',$buildDir,$proj)
  if ($c.cmake.Count) { $westArgs += @('--') + $c.cmake }
  Push-Location $proj
  try {
    west @westArgs
    if ($LASTEXITCODE -ne 0) { throw "west build failed for '$name' (exit $LASTEXITCODE)" }
  } finally {
    Pop-Location
  }
}

if ($Target -eq 'all') {
  foreach ($name in $configs.Keys) { Invoke-BuildConfig $name }
} else {
  Invoke-BuildConfig $Target
}

Write-Host "Done." -ForegroundColor Green