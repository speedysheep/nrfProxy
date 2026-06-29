#!/usr/bin/env bash
#
# Recreate the nRF Connect build configurations for nrfProxy (Linux/macOS).
#
# The build*/ directories are generated output (gitignored). This script is the
# committed, machine-portable definition of every build configuration: board
# target, build dir, and the per-board flags that are easy to forget
# (--no-sysbuild for the Pro Micro, EXTRA_CONF_FILE=prod.conf for production).
# Running a target reproduces its build*/ dir, which the IDE then picks up.
#
# See CLAUDE.md for the full rationale behind each flag.
#
# Usage:
#   ./build.sh                # build every configuration
#   ./build.sh promicro       # build only the Pro Micro (nice!nano) UF2 config
#   ./build.sh xiao_prod      # production XIAO (no logging/USB console)
#
# Prerequisites: `west` must be on PATH and the NCS environment activated, e.g.
#   nrfutil toolchain-manager launch --ncs-version v3.3.1 --shell
#   # or: source ~/ncs/v3.3.1/zephyr/zephyr-env.sh && source <your venv>/bin/activate
# Override the project / workspace locations with env vars if they differ:
#   PROJ=~/projects/nrfProxy  NCS=~/ncs/v3.3.1  ./build.sh

set -euo pipefail

PROJ="${PROJ:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
NCS="${NCS:-$HOME/ncs/v3.3.1}"

if ! command -v west >/dev/null 2>&1; then
  echo "error: 'west' not on PATH. Activate the NCS environment first (see header)." >&2
  exit 1
fi

# --- build configuration table -----------------------------------------------
<<<<<<< Updated upstream
# Each entry: <board>|<build-dir>|<extra args (space-separated, may be empty)>
declare -A configs=(
  [dk]="nrf52840dk/nrf52840|build|"
  [xiao]="xiao_ble/nrf52840|build_xiao|"
  [xiao_prod]="xiao_ble/nrf52840|build_xiao_prod|-- -DEXTRA_CONF_FILE=prod.conf"
  [promicro]="promicro_nrf52840/nrf52840/uf2|build_promicro|--no-sysbuild"
  [promicro_prod]="promicro_nrf52840/nrf52840/uf2|build_promicro_prod|--no-sysbuild -- -DEXTRA_CONF_FILE=prod.conf"
  [dongle]="nrf52840dongle/nrf52840|build_dongle|"
=======
# Each entry: <board>|<build-dir>|<west args>|<cmake args>
# Source dir must precede '--'; west args go before it, cmake args after.
declare -A configs=(
  [dk]="nrf52840dk/nrf52840|build||"
  [xiao]="xiao_ble/nrf52840|build_xiao||"
  [xiao_prod]="xiao_ble/nrf52840|build_xiao_prod||-DEXTRA_CONF_FILE=prod.conf"
  [promicro]="promicro_nrf52840/nrf52840/uf2|build_promicro|--no-sysbuild|"
  [promicro_prod]="promicro_nrf52840/nrf52840/uf2|build_promicro_prod|--no-sysbuild|-DEXTRA_CONF_FILE=prod.conf"
  [dongle]="nrf52840dongle/nrf52840|build_dongle||"
>>>>>>> Stashed changes
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

<<<<<<< Updated upstream
  local board dir extra
  IFS='|' read -r board dir extra <<< "$spec"

  echo "==> Building '$name' : $board -> $dir"
  # west build must run from inside the NCS workspace so the 'nrf' module is in scope.
  ( cd "$NCS" && west build -b "$board" --pristine -d "$PROJ/$dir" $extra "$PROJ" )
=======
  local board dir west_args cmake_args
  IFS='|' read -r board dir west_args cmake_args <<< "$spec"

  # Source dir ($PROJ) before '--'; CMake args after it.
  local sep=()
  [[ -n "$cmake_args" ]] && sep=(-- $cmake_args)

  echo "==> Building '$name' : $board -> $dir"
  # west build must run from inside the NCS workspace so the 'nrf' module is in scope.
  ( cd "$NCS" && west build -b "$board" --pristine -d "$PROJ/$dir" $west_args "$PROJ" ${sep[@]+"${sep[@]}"} )
>>>>>>> Stashed changes
}

target="${1:-all}"
if [[ "$target" == "all" ]]; then
  for name in "${order[@]}"; do build_config "$name"; done
else
  build_config "$target"
fi

echo "Done."