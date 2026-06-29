# nrfProxy

A Zephyr / nRF Connect SDK application that bridges a serial UART to a phone over
Bluetooth LE, acting as a **bidirectional proxy**:

Note this project is almost entirely written by Claude. Progress is wonderful! 

"Production" builds are just the regular build, but with optimizations enabled and debug console disabled. This is to limit power drain.

- **Serial → phone:** bytes received on UART1 are forwarded as BLE notifications via the
  Nordic UART Service (NUS).
- **Phone → serial:** bytes written to the NUS RX characteristic are sent out UART1.

UART1 is the data channel on every board; the default baud rate is **115200** (share a
common ground with your serial source). Connect and test with the *nRF Connect for Mobile*
or *nRF Toolbox* apps.

## Supported boards

Each board has its own devicetree overlay and (optionally) a Kconfig fragment under
[`boards/`](boards/), which Zephyr auto-merges on top of the shared [`prj.conf`](prj.conf).
The filename must match the full normalized board target (with `/` → `_`, including any
variant suffix).

| Board | Build target | Overlay + conf | UART1 (TX / RX) | Flashing |
|-------|--------------|----------------|-----------------|----------|
| nRF52840 DK | `nrf52840dk/nrf52840` | `nrf52840dk_nrf52840.{overlay,conf}` | P1.02 / P1.01 | J-Link (`west flash`) |
| Seeed XIAO BLE | `xiao_ble/nrf52840` | `xiao_ble_nrf52840.{overlay,conf}` | D0/P0.02 / D1/P0.03 | UF2 drag-and-drop |
| Pro Micro nRF52840 / nice!nano | `promicro_nrf52840/nrf52840/uf2` | `promicro_nrf52840_nrf52840_uf2.{overlay,conf}` | P0.06 / P0.08 | UF2 drag-and-drop |
| Nordic nRF52840 Dongle (PCA10059) | `nrf52840dongle/nrf52840` | `nrf52840dongle_nrf52840.{overlay,conf}` | P0.15 / P0.13 | USB DFU |

The `.overlay` enables UART1 and assigns its pins; the `.conf` holds board-specific Kconfig
(debug vs. size optimization, USB buffer tuning, etc.). The shared config lives in
`prj.conf`; put board-only settings in the per-board `.conf`, not in `prj.conf`.

## Building

The nRF Connect SDK (v3.3.1) provides `west` and the toolchain. The easiest way to build is
the wrapper scripts at the repo root — they bake in the per-board build directory and flags
(the toolchain environment on Windows, `-DSB_CONFIG_PARTITION_MANAGER=n` for DTS
partitioning, and `EXTRA_CONF_FILE=prod.conf` for production targets):

```powershell
# Windows (PowerShell)
.\build.ps1            # build every configuration
.\build.ps1 xiao      # build just the XIAO BLE
```

```bash
# Linux / macOS — activate the NCS environment first, then:
./build.sh            # build every configuration
./build.sh xiao       # build just the XIAO BLE
```

#### Toolchain / SDK / project locations

The scripts default to the standard install paths, but you can point them elsewhere. Each
location is resolved as **(1) a parameter passed to the script → (2) an environment
variable → (3) a built-in default**, in that order. The project path defaults to the
directory the script lives in, so a checked-out repo is relocatable without any
configuration.

| What | Default | PowerShell parameter | Bash flag | Environment variable |
|------|---------|----------------------|-----------|----------------------|
| NCS toolchain dir (e.g. `…/toolchains/936afb6332`) | Windows: `C:\ncs\toolchains\936afb6332`; Bash: unset (uses an already-activated env) | `-Toolchain` | `--toolchain` | `NRFPROXY_TOOLCHAIN` |
| NCS workspace (the `vX.Y.Z` dir) | `C:\ncs\v3.3.1` / `~/ncs/v3.3.1` | `-Ncs` | `--ncs` | `NRFPROXY_NCS` |
| This project | the script's own directory | `-Proj` | `--proj` | `NRFPROXY_PROJ` |

```powershell
# Windows: override per run via parameters…
.\build.ps1 dk -Toolchain D:\ncs\toolchains\936afb6332 -Ncs D:\ncs\v3.3.1
# …or via environment variables (parameters win over these):
$env:NRFPROXY_NCS = 'D:\ncs\v3.3.1'; .\build.ps1
```

```bash
# Linux / macOS: flags…
./build.sh dk --ncs ~/ncs/v3.3.1
# …or env vars. Pass --toolchain / NRFPROXY_TOOLCHAIN to prepend the toolchain's bin dirs
# to PATH instead of activating the NCS environment beforehand:
NRFPROXY_NCS=~/ncs/v3.3.1 ./build.sh
```

### Targets

| Target | Board | Build dir | Notes |
|--------|-------|-----------|-------|
| `dk` | nRF52840 DK | `build_devkit/` | debug build |
| `xiao` | XIAO BLE | `build_xiao/` | debug build (USB console + logging) |
| `xiao_prod` | XIAO BLE | `build_xiao_prod/` | production: no logging / USB console |
| `promicro` | Pro Micro / nice!nano | `build_promicro/` | debug build |
| `promicro_prod` | Pro Micro / nice!nano | `build_promicro_prod/` | production: no logging / USB console |
| `dongle` | nRF52840 Dongle | `build_dongle/` | debug build |
| `all` *(default)* | — | all of the above | builds everything |

The build directories are generated output (gitignored); the wrapper scripts are the
committed source of truth for each configuration. Each rebuild is pristine.

### Flashing

The flashable image is under `<build dir>/nrfProxy/zephyr/`

- **DK:** `west flash -d build_devkit`, or drag `build_devkit/nrfProxy/zephyr/zephyr.hex`
  onto the J-Link drive.
- **XIAO / Pro Micro:** double-tap RESET to mount the bootloader's USB drive, then copy
  `<build dir>/nrfProxy/zephyr/zephyr.uf2` onto it.
- **Dongle:** put it in bootloader mode (side RESET button), then write
  `build_dongle/nrfProxy/zephyr/zephyr.hex` via *nRF Connect for Desktop → Programmer*, or
  package it into a DFU zip and flash over USB serial with `nrfutil`.

> **Deep build/flash details** (toolchain environment setup, per-board flash offsets, the
> reason Partition Manager is disabled, and board-specific gotchas) live in
> [`CLAUDE.md`](CLAUDE.md).

## Logging

Logging goes to each board's default console, which is board-dependent:

- **nRF52840 DK:** the J-Link VCOM serial port (UART0).
- **XIAO BLE, Pro Micro, and Dongle:** a **USB CDC-ACM** serial port (no debug UART). On
  the XIAO and Pro Micro boards the production build (`*_prod`) drops the console and logging entirely.

## Per-board Kconfig fragments

Debug/optimization settings are board-specific and live in the per-board fragments that
Zephyr auto-merges on top of `prj.conf`:

| Fragment | Build |
|----------|-------|
| `boards/nrf52840dk_nrf52840.overlay` + `boards/nrf52840dk_nrf52840.conf` | debug (`-Og`, thread info) |
| `boards/xiao_ble_nrf52840.overlay` + `boards/xiao_ble_nrf52840.conf` | debug (USB DFU) |
| `boards/promicro_nrf52840_nrf52840_uf2.overlay` + `boards/promicro_nrf52840_nrf52840_uf2.conf` | debug (UF2, USB DFU) |
| `boards/nrf52840dongle_nrf52840.overlay` + `boards/nrf52840dongle_nrf52840.conf` | debug (USB DFU) |

For a logging-free **production** build on the XIAO (or Pro Micro), layer
[`prod.conf`](prod.conf) on top via `EXTRA_CONF_FILE` — the `xiao_prod` / `promicro_prod` wrapper targets do this for you.