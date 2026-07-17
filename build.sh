#!/usr/bin/env bash
#
# Recreate the nRF Connect build configurations for nrfProxy (Linux/macOS).
#
# The build*/ directories are generated output (gitignored). This script is the
# committed, machine-portable definition of every build configuration: board
# target, build dir, and the per-board flags that are easy to forget
# (-DSB_CONFIG_PARTITION_MANAGER=n for DTS partitioning, EXTRA_CONF_FILE=prod.conf
# for production). Running a target reproduces its build*/ dir, which the IDE picks up.
#
# All boards disable the (deprecated) nRF Connect SDK Partition Manager via
# -DSB_CONFIG_PARTITION_MANAGER=n, so the flash layout comes from the board's
# devicetree partitions (DTS partitioning). sysbuild itself is kept (west flash, the
# nrfProxy/ image subdir); note that with a single image and PM off there is no
# merged.hex. See CLAUDE.md ("DTS partitioning") for the full rationale and the
# resulting per-board flash offsets.
#
# Usage:
#   ./build.sh                        # build every configuration
#   ./build.sh promicro               # build only the Pro Micro (nice!nano) UF2 config
#   ./build.sh xiao_prod              # production XIAO (no logging/USB console)
#   ./build.sh dk --ncs ~/ncs/v3.3.1  # override the workspace location for one run
#
# Locations are resolved as: (1) command-line flag, (2) environment variable,
# (3) a fallback default — in that order. Flags / env vars:
#   --proj      PATH   NRFPROXY_PROJ        this project (default: this script's dir)
#   --ncs       PATH   NRFPROXY_NCS         NCS workspace (default: ~/ncs/v3.3.1)
#   --toolchain PATH   NRFPROXY_TOOLCHAIN   NCS toolchain dir; if set, its bin dirs are
#                                           prepended to PATH (default: unset — rely on an
#                                           already-activated NCS environment)
#
# Prerequisites: `west` must be on PATH (either activate the NCS environment first, e.g.
#   nrfutil toolchain-manager launch --ncs-version v3.3.1 --shell
#   # or: source ~/ncs/v3.3.1/zephyr/zephyr-env.sh && source <your venv>/bin/activate
# or point --toolchain / NRFPROXY_TOOLCHAIN at the toolchain dir so this script adds it).

set -euo pipefail

# Defaults come from env vars (NRFPROXY_*); command-line flags below override them.
PROJ="${NRFPROXY_PROJ:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
NCS="${NRFPROXY_NCS:-$HOME/ncs/v3.3.1}"
TOOLCHAIN="${NRFPROXY_TOOLCHAIN:-}"

target="all"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --proj)      PROJ="$2";      shift 2 ;;
    --ncs)       NCS="$2";       shift 2 ;;
    --toolchain) TOOLCHAIN="$2"; shift 2 ;;
    -*)          echo "error: unknown option '$1'" >&2; exit 2 ;;
    *)           target="$1";    shift ;;
  esac
done

# If a toolchain dir was given, prepend its bin dirs so `west` is found without a
# separately-activated environment. Harmless if west is already on PATH.
if [[ -n "$TOOLCHAIN" ]]; then
  export PATH="$TOOLCHAIN/bin:$TOOLCHAIN/opt/bin:$TOOLCHAIN/opt/bin/Scripts:$TOOLCHAIN/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
fi

if ! command -v west >/dev/null 2>&1; then
  echo "error: 'west' not on PATH. Activate the NCS environment or pass --toolchain (see header)." >&2
  exit 1
fi

# --- build configuration table -----------------------------------------------
# Each entry: <board>|<build-dir>|<cmake args (after --)>
# Source dir must precede '--'; everything after '--' goes to sysbuild/CMake.
# Every config passes -DSB_CONFIG_PARTITION_MANAGER=n to disable the deprecated
# Partition Manager and link from the devicetree code_partition (DTS partitioning),
# while keeping sysbuild itself.
pm_off="-DSB_CONFIG_PARTITION_MANAGER=n"
declare -A configs=(
  [dk]="nrf52840dk/nrf52840|build_devkit|$pm_off"
  [xiao]="xiao_ble/nrf52840|build_xiao|$pm_off"
  [xiao_prod]="xiao_ble/nrf52840|build_xiao_prod|$pm_off -DEXTRA_CONF_FILE=prod.conf"
  [promicro]="promicro_nrf52840/nrf52840/uf2|build_promicro|$pm_off"
  [promicro_prod]="promicro_nrf52840/nrf52840/uf2|build_promicro_prod|$pm_off -DEXTRA_CONF_FILE=prod.conf"
  [dongle]="nrf52840dongle/nrf52840|build_dongle|$pm_off"
)

# Preserve a sensible build order (associative arrays are unordered).
order=(dk xiao xiao_prod promicro promicro_prod dongle)

build_config() {
  local name="$1"
  local spec="${configs[$name]:-}"
  if [[ -z "$spec" ]]; then
    echo "error: unknown target '$name' (valid: ${order[*]} all)" >&2
    exit 2
  fi

  local board dir cmake_args
  IFS='|' read -r board dir cmake_args <<< "$spec"

  # Source dir ($PROJ) before '--'; sysbuild/CMake args after it. cmake_args is a
  # flag list, so split it on whitespace into real array elements.
  local sep=()
  if [[ -n "$cmake_args" ]]; then
    local args=()
    read -ra args <<< "$cmake_args"
    sep=(-- "${args[@]}")
  fi

  echo "==> Building '$name' : $board -> $dir"
  # west build must run from inside the NCS workspace so the 'nrf' module is in scope.
  ( cd "$NCS" && west build -b "$board" --pristine -d "$PROJ/$dir" "$PROJ" ${sep[@]+"${sep[@]}"} )
}

if [[ "$target" == "all" ]]; then
  for name in "${order[@]}"; do build_config "$name"; done
else
  build_config "$target"
fi

echo "Done."