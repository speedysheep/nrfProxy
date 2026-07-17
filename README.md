# nrfProxy

[![CI](https://github.com/speedysheep/nrfProxy/actions/workflows/ci.yml/badge.svg)](https://github.com/speedysheep/nrfProxy/actions/workflows/ci.yml)

A Zephyr / nRF Connect SDK application that bridges a serial UART to a phone over
Bluetooth LE, acting as a **bidirectional proxy**:

- **Serial → phone:** bytes received on UART1 are forwarded as BLE notifications via the
  Nordic UART Service (NUS).
- **Phone → serial:** bytes written to the NUS RX characteristic are sent out UART1.

UART1 is the data channel on every board; the default baud rate is **115200** (share a
common ground with your serial source). Connect and test with the *nRF Connect for Mobile*
or *nRF Toolbox* apps.

Note this project is almost entirely written by Claude. Progress is wonderful!

"Production" builds are just the regular build, but with optimizations enabled and the
debug console disabled, to limit power drain.

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

The defaults are just the *standard* install paths — the SDK does not have to live on C:, and
putting it elsewhere needs no change to these scripts (set `NRFPROXY_NCS` /
`NRFPROXY_TOOLCHAIN` once and `.\build.ps1` works unqualified). See
[`CLAUDE.md`](CLAUDE.md) for installing to another drive, and for the two Windows traps that
come with it: cross-drive `west build` and the 260-character path limit.
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

"Debug" vs. "production" above describes the **build**, not the board. The XIAO BLE is the
production *board* (see `boards/xiao_ble_nrf52840.conf`): its `xiao_prod` build is the
shipping configuration, while the plain `xiao` build is the same board with logging and the
USB console kept for bench work. The DK and Dongle are bench/debug boards only.

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

## Testing

Every push and pull request runs [`.github/workflows/ci.yml`](.github/workflows/ci.yml)
inside Nordic's pinned toolchain container (`ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.3.1`),
which materialises an NCS workspace and builds this repo as an out-of-tree application —
the same flow as the wrapper scripts, just automated.

| Layer | What it protects | Run it locally |
|-------|------------------|----------------|
| Build matrix | All six configurations compile | `.\build.ps1` / `./build.sh` |
| Config assertions | Flash offsets, Partition Manager stays off, async-UART gating, `prod.conf` not stripping the pairing lock | `python scripts/check_configs.py <target> <build dir>` |
| Unit tests | The pure logic in `src/proxy_core.c` (hooks, identity derivation, advertising/send policy) | `west twister -T app/tests/unit -p native_sim` |
| Integration tests | The UART data path in `src/uart_bridge.c` (ring flow, TX chaining, RX recovery) | `west twister -T app/tests/integration -p native_sim` |

Unit and integration tests run on Zephyr's **`native_sim`** board, which is **Linux-only** —
on a Windows development machine run them under WSL or let CI run them; the firmware build
matrix is the part that runs natively on Windows.

Hardware behaviour (pairing dialogs, reconnect, throughput) is still verified by hand; see
[`ADD_TESTING_PLAN.md`](ADD_TESTING_PLAN.md) for the full plan, the test IDs, and what is
deliberately out of scope.

## Logging

Logging goes to each board's default console, which is board-dependent:

- **nRF52840 DK:** the J-Link VCOM serial port (UART0).
- **XIAO BLE, Pro Micro, and Dongle:** a **USB CDC-ACM** serial port (no debug UART). On
  the XIAO and Pro Micro boards the production build (`*_prod`) drops the console and logging entirely.

## Pairing / security lock

The bridge **locks to the first phone that pairs with it**. Pairing is initiated by the
**central** (the connecting app), which shows the system **Just Works pairing dialog** (no
PIN) — the firmware deliberately doesn't send its own SMP Security Request, as that made some
Android phones pop two dialogs for one bond. A generic BLE client (e.g. nRF Connect) must
therefore start pairing itself. Once paired, the bond is stored in flash and:

- Only the bonded phone can reconnect — the device advertises with a link-layer **filter
  accept list**, so other phones can still *see* it in a scan but their connection attempts
  are ignored.
- Serial data flows **only after the link is encrypted**. A link that never encrypts is
  disconnected (after ~10 s once locked; first-time pairing gets ~60 s so there's time to
  accept the pairing dialog).
- The bond **survives power loss** and re-pairs automatically on reconnect (no new dialog).

**Factory reset (hand the device to a new phone):** hold the board's **bond-reset button
while powering it on** (through boot, ~3 s — the advertising LED blinks as feedback). This
wipes the stored bond and returns to open pairing mode. If a phone is factory-reset or
replaced, this button is the only recovery path.

| Board | Bond-reset button | Wiring |
|-------|-------------------|--------|
| nRF52840 DK | **Button 1** (SW1) | on-board — nothing to wire |
| nRF52840 Dongle | **SW1** (top-face button, *not* the side RESET) | on-board — nothing to wire |
| XIAO BLE | **D2 / P0.28** | momentary switch (or tweezer short) from the D2 pad to GND |
| Pro Micro / nice!nano | **P0.17** | momentary switch from the P0.17 pad to GND (sits below the left-edge GND pads) |

All use internal pull-ups (active-low), so no external resistor is needed. A board whose
overlay omits the `bond-reset` alias simply has no reset button. See [`CLAUDE.md`](CLAUDE.md)
and [`PAIRING_PLAN.md`](PAIRING_PLAN.md) for the full design.

## Per-board Kconfig fragments

Debug/optimization settings are board-specific and live in the per-board fragments that
Zephyr auto-merges on top of `prj.conf`:

| Fragment | Build |
|----------|-------|
| `boards/nrf52840dk_nrf52840.overlay` + `boards/nrf52840dk_nrf52840.conf` | debug (`-Og`, thread info) |
| `boards/xiao_ble_nrf52840.overlay` + `boards/xiao_ble_nrf52840.conf` | debug (UF2 drag-and-drop) |
| `boards/promicro_nrf52840_nrf52840_uf2.overlay` + `boards/promicro_nrf52840_nrf52840_uf2.conf` | debug (UF2 drag-and-drop) |
| `boards/nrf52840dongle_nrf52840.overlay` + `boards/nrf52840dongle_nrf52840.conf` | debug (USB DFU) |

For a logging-free **production** build on the XIAO (or Pro Micro), layer
[`prod.conf`](prod.conf) on top via `EXTRA_CONF_FILE` — the `xiao_prod` / `promicro_prod` wrapper targets do this for you.