<#
.SYNOPSIS
  Recreate the nRF Connect build configurations for nrfProxy (Windows).

.DESCRIPTION
  The build*/ directories are generated output (gitignored). This script is the
  committed, machine-portable definition of every build configuration: board
  target, build dir, and the per-board flags that are easy to forget
  (--no-sysbuild for the Pro Micro, EXTRA_CONF_FILE=prod.conf for production).
  Running a target reproduces its build*/ dir, which the IDE then picks up.

  See CLAUDE.md for the full rationale behind each flag.

.EXAMPLE
  .\build.ps1                 # build every configuration
  .\build.ps1 promicro        # build only the Pro Micro (nice!nano) UF2 config
  .\build.ps1 xiao_prod       # production XIAO (no logging/USB console)
#>
param(
  [ValidateSet('dk','xiao','xiao_prod','promicro','promicro_prod','dongle','all')]
  [string]$Target = 'all'
)

$ErrorActionPreference = 'Stop'

# --- toolchain / SDK environment (NCS v3.3.1, toolchain 936afb6332) -----------
$tc = "C:\ncs\toolchains\936afb6332"
$env:PATH = "$tc;$tc\mingw64\bin;$tc\bin;$tc\opt\bin;$tc\opt\bin\Scripts;$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$tc\opt\zephyr-sdk\riscv64-zephyr-elf\bin;$env:PATH"
$env:PYTHONPATH = "$tc\opt\bin;$tc\opt\bin\Lib;$tc\opt\bin\Lib\site-packages"
$env:NRFUTIL_HOME = "$tc\nrfutil\home"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"

$proj = "C:\Users\extra\projects\nrfProxy"
$ncs  = "C:\ncs\v3.3.1"

# --- build configuration table -----------------------------------------------
<<<<<<< Updated upstream
# key => @{ board; dir; extra (array of extra west/cmake args) }
$configs = [ordered]@{
  dk            = @{ board = 'nrf52840dk/nrf52840';          dir = 'build';              extra = @() }
  xiao          = @{ board = 'xiao_ble/nrf52840';            dir = 'build_xiao';         extra = @() }
  xiao_prod     = @{ board = 'xiao_ble/nrf52840';            dir = 'build_xiao_prod';    extra = @('--','-DEXTRA_CONF_FILE=prod.conf') }
  promicro      = @{ board = 'promicro_nrf52840/nrf52840/uf2'; dir = 'build_promicro';      extra = @('--no-sysbuild') }
  promicro_prod = @{ board = 'promicro_nrf52840/nrf52840/uf2'; dir = 'build_promicro_prod'; extra = @('--no-sysbuild','--','-DEXTRA_CONF_FILE=prod.conf') }
  dongle        = @{ board = 'nrf52840dongle/nrf52840';      dir = 'build_dongle';       extra = @() }
}

function Build-Config([string]$name) {
=======
# key => @{ board; dir; west (args before the source dir); cmake (args after --) }
# Source dir must precede '--'; everything after '--' goes to CMake, not west.
$configs = [ordered]@{
  dk            = @{ board = 'nrf52840dk/nrf52840';          dir = 'build';              west = @();                cmake = @() }
  xiao          = @{ board = 'xiao_ble/nrf52840';            dir = 'build_xiao';         west = @();                cmake = @() }
  xiao_prod     = @{ board = 'xiao_ble/nrf52840';            dir = 'build_xiao_prod';    west = @();                cmake = @('-DEXTRA_CONF_FILE=prod.conf') }
  promicro      = @{ board = 'promicro_nrf52840/nrf52840/uf2'; dir = 'build_promicro';      west = @('--no-sysbuild'); cmake = @() }
  promicro_prod = @{ board = 'promicro_nrf52840/nrf52840/uf2'; dir = 'build_promicro_prod'; west = @('--no-sysbuild'); cmake = @('-DEXTRA_CONF_FILE=prod.conf') }
  dongle        = @{ board = 'nrf52840dongle/nrf52840';      dir = 'build_dongle';       west = @();                cmake = @() }
}

function Invoke-BuildConfig([string]$name) {
>>>>>>> Stashed changes
  $c = $configs[$name]
  $buildDir = Join-Path $proj $c.dir
  Write-Host "==> Building '$name' : $($c.board) -> $($c.dir)" -ForegroundColor Cyan

  # west build must run from inside the NCS workspace so the 'nrf' module is in scope.
<<<<<<< Updated upstream
  $args = @('build','-b',$c.board,'--pristine','-d',$buildDir) + $c.extra + @($proj)
  Push-Location $ncs
  try {
    west @args
=======
  # Source dir ($proj) before '--'; CMake args after it.
  $westArgs = @('build','-b',$c.board,'--pristine','-d',$buildDir) + $c.west + @($proj)
  if ($c.cmake.Count) { $westArgs += @('--') + $c.cmake }
  Push-Location $ncs
  try {
    west @westArgs
>>>>>>> Stashed changes
    if ($LASTEXITCODE -ne 0) { throw "west build failed for '$name' (exit $LASTEXITCODE)" }
  } finally {
    Pop-Location
  }
}

if ($Target -eq 'all') {
<<<<<<< Updated upstream
  foreach ($name in $configs.Keys) { Build-Config $name }
} else {
  Build-Config $Target
=======
  foreach ($name in $configs.Keys) { Invoke-BuildConfig $name }
} else {
  Invoke-BuildConfig $Target
>>>>>>> Stashed changes
}

Write-Host "Done." -ForegroundColor Green