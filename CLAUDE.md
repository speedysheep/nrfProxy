# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`nrfProxy` is a Zephyr / nRF Connect SDK application that bridges a serial UART to a
phone over Bluetooth LE, acting as a **bidirectional proxy**:

- **Serial → phone:** bytes received on UART1 are forwarded as BLE notifications via the
  Nordic UART Service (NUS).
- **Phone → serial:** bytes written to the NUS RX characteristic are sent out UART1.

Supported boards (each has its own overlay in `boards/`): **nRF52840 DK**
(`nrf52840dk/nrf52840`), **Seeed XIAO BLE** (`xiao_ble/nrf52840`), the
**"Pro Micro nRF52840" / nice!nano clone** (`promicro_nrf52840/nrf52840/uf2` — a generic
nRF52840 Pro Micro board with the Adafruit/nice!nano UF2 bootloader), and the
**Nordic nRF52840 Dongle** (`nrf52840dongle/nrf52840`, PCA10059 — debug/bench, flashed by
USB DFU). UART1 is the data channel on all; `main.c` is board-agnostic (references
`DT_NODELABEL(uart1)`). Connect/test with the *nRF Connect for Mobile* or *nRF Toolbox*
apps.

## Files

- `src/main.c` — entire application (UART async driver, BLE/NUS, the bridge logic).
- `prj.conf` — Kconfig: BLE peripheral + NUS, async UART, MTU/throughput tuning, debug flags.
- `prod.conf` — optional **production** fragment for the XIAO (battery use): drops logging
  and the USB CDC-ACM console. Applied via `EXTRA_CONF_FILE`, *not* auto-merged — see
  "Production build" below.
- `boards/<board>_<qualifier>.overlay` — per-board: enables UART1 and assigns its pins
  (`nrf52840dk_nrf52840.overlay`, `xiao_ble_nrf52840.overlay`,
  `promicro_nrf52840_nrf52840_uf2.overlay`, `nrf52840dongle_nrf52840.overlay`). Add one
  per new board. **The filename must
  match the *full* normalized board target** (`/`→`_`), including any variant: the
  promicro is built as `promicro_nrf52840/nrf52840/uf2`, so its overlay/conf carry the
  `_uf2` suffix — naming them `promicro_nrf52840_nrf52840.*` (no variant) is silently
  ignored and the overlay won't apply (uart1 stays disabled → `DEVICE_DT_GET` build
  error). The XIAO/DK have no variant, hence no suffix.
- `CMakeLists.txt` — standard Zephyr app boilerplate; only `src/main.c` is compiled.
- `README.md` — user-facing overview: supported-board table, how to run the `build.ps1`/
  `build.sh` wrappers, flashing, and the per-board Kconfig-fragment list. This file
  (`CLAUDE.md`) stays the deep reference (build-env setup, flash offsets, gotchas).
- `.mcp.json` — Memfault MCP server config (unrelated to firmware).

There is no test suite and no Cursor/Copilot rules.

## Build environment (IMPORTANT — read before building)

The nRF Connect SDK is installed at `C:\ncs\v3.3.1` (Zephyr 4.3.99) with toolchain
`936afb6332`. `west`, `cmake`, etc. are **not on PATH** by default. Set the environment,
then build **from inside the NCS workspace** so the `nrf` module is in scope:

```powershell
$proj = (Resolve-Path .).Path     # run this from your nrfProxy checkout
$tc = "C:\ncs\toolchains\936afb6332"
$env:PATH = "$tc;$tc\mingw64\bin;$tc\bin;$tc\opt\bin;$tc\opt\bin\Scripts;$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$tc\opt\zephyr-sdk\riscv64-zephyr-elf\bin;$env:PATH"
$env:PYTHONPATH = "$tc\opt\bin;$tc\opt\bin\Lib;$tc\opt\bin\Lib\site-packages"
$env:NRFUTIL_HOME = "$tc\nrfutil\home"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
Set-Location "C:\ncs\v3.3.1"
west build -b nrf52840dk/nrf52840 -d "$proj\build_devkit" "$proj" -- -DSB_CONFIG_PARTITION_MANAGER=n
```

In the commands below, `$proj` is the path to this checkout (set as above); `C:\ncs\*` are
the standard nRF Connect SDK install locations (the `build.ps1`/`build.sh` wrappers let you
override all three via the `NRFPROXY_PROJ` / `NRFPROXY_NCS` / `NRFPROXY_TOOLCHAIN` env vars
or their parameters — see the README).

- **DTS partitioning (`-DSB_CONFIG_PARTITION_MANAGER=n`).** Every build disables the
  nRF Connect SDK **Partition Manager** — which is *deprecated* in NCS v3.3.1 — so the flash
  layout comes from each board's **devicetree partitions** (the `code_partition` /
  `FLASH_LOAD_OFFSET`), not a PM-generated `partitions.yml`. sysbuild itself is **kept**
  (so `west flash` and the `nrfProxy/` image subdir still work); only PM is turned off. The
  flag is a *sysbuild* Kconfig, so it goes after `--`. The `build.ps1`/`build.sh` wrappers
  pass it for all targets; pass it yourself for any manual `west build`. Verify it took
  effect: there is **no `partitions.yml`** in the build dir, and the per-board offset is
  correct in `build*/nrfProxy/zephyr/.config` (DK `0x0`, XIAO `0x27000`, Pro Micro
  `0x26000`, Dongle `0x1000`).
- Add `--pristine` after a config/overlay change or to clear a bad cache.
- Incremental rebuild (after editing `src/main.c` only): drop `-b` and `--pristine`,
  keep `-d <build>` (the `SB_CONFIG_*` flag is remembered in the CMake cache; a `--pristine`
  rebuild must pass it again).
- Flash: `west flash -d build_devkit`. With PM off there is **no `merged.hex`** (a single image has
  nothing to merge) — the flashable image is `build_devkit/nrfProxy/zephyr/zephyr.hex`; drag that
  onto the J-Link drive instead.
- First full build takes a few minutes (BLE stack); run it in the background.
- **Convenience wrappers** that recreate every per-board build configuration (board +
  build dir + the per-board flags below) live at repo root: `build.ps1` (Windows; bakes in
  the toolchain env block above) and `build.sh` (Linux/macOS; assumes `west` is already on
  PATH via the NCS env). Run with a target — `dk`, `xiao`, `xiao_prod`, `promicro`,
  `promicro_prod`, `dongle`, or `all` (default), e.g. `.\build.ps1 promicro`. These are the
  committed source of truth for the build configs since the `build*/` dirs are gitignored.

### nice!nano / Pro Micro nRF52840 clone (UF2 flashing — no debugger)
This board has **no debugger** — it ships with the Adafruit/nice!nano UF2 bootloader.
Build the **`uf2` variant** so the app lands at flash offset `0x26000` (the bootloader
reserves the SoftDevice s140 v6 slot below it; we don't use a SoftDevice but the offset
must still match). Like every board here it builds with **Partition Manager disabled**
(`-DSB_CONFIG_PARTITION_MANAGER=n`) so the offset comes from the DT `code_partition` (see
"DTS partitioning" above) — without that the app is silently linked at `0x0` and the board
reboot-loops (see the gotcha below):

```powershell
west build -b promicro_nrf52840/nrf52840/uf2 --pristine `
  -d "$proj\build_promicro" "$proj" `
  -- -DSB_CONFIG_PARTITION_MANAGER=n
```

- **THE #1 PRO-MICRO GOTCHA — Partition Manager links the app at `0x0` (must be off).**
  `promicro_nrf52840` is a community `others/` board target: it carries the plain-Zephyr
  DT `code_partition` at `0x26000` but **no nRF Connect SDK Partition Manager metadata**.
  When PM is enabled (the historical NCS default), with no PM layout for this board PM
  defaults the `app` partition to **`address: 0x0`, full flash** (it would show up in
  `build_promicro/partitions.yml`). The image is then *linked to run from 0x0* but the UF2
  step still copies it to `0x26000`, so the reset vector points into low flash → instant
  HardFault on every boot → **reboot loop that looks like a rapid LED flash, app never
  runs, USB CDC port never enumerates** (verify: 1st UF2 block's reset vector must be
  inside `0x26000..0xc6000`, *not* `0x000xxxxx`). Disabling PM bypasses this entirely and
  uses the DT `code_partition` (`CONFIG_USE_DT_CODE_PARTITION=y`, already in the board's
  `uf2` defconfig) → correctly linked at `0x26000` (verified: `partitions.yml` absent,
  `CONFIG_FLASH_LOAD_OFFSET=0x26000`, UF2 first-block target `0x26000`). **Historical
  note:** this board used to be the *only* one built `--no-sysbuild` to dodge PM; now PM is
  disabled SDK-wide via the sysbuild Kconfig (`SB_CONFIG_PARTITION_MANAGER=n` — PM is
  *deprecated* in NCS v3.3.1), so the special case is gone and all boards share one
  mechanism while keeping sysbuild's conveniences. The XIAO/Dongle ship PM metadata so they
  built correctly *with* PM too, but DTS partitioning is now the uniform path; the DK has no
  bootloader (app at 0x0) so its offset is 0x0 either way.
- **Flash by drag-and-drop, not `west flash`:** double-tap the board's RESET button to
  mount the bootloader's USB mass-storage drive, then copy the UF2 onto it. sysbuild is
  kept (only PM is off), so the artifact is in the image subdir at
  **`build_promicro/nrfProxy/zephyr/zephyr.uf2`**.
- Console/logs are over **USB CDC-ACM** (same as the XIAO — same log-visibility quirk,
  same `CONFIG_UDC_BUF_POOL_SIZE` bump in the board `.conf`). For battery use, layer
  `prod.conf` via `EXTRA_CONF_FILE` exactly like the XIAO.
- **One LED only** (`led0` = P0.15, active-high): the overlay maps both `led-connected`
  and `led-advertising` to it (mutually-exclusive states, so unambiguous) and
  **deliberately leaves `led-error` unmapped**, so the error state lights no LED (by
  request) — `main.c`'s `GPIO_DT_SPEC_GET_OR` null-fallback handles the missing alias.
- **DC/DC regulator is ENABLED** (`&reg1` DC/DC block in the overlay): the genuine
  nice!nano (confirmed `Board-ID: nRF52840-nicenano`) has the DCC/DEC4 inductor, so
  DC/DC is safe and cuts active/radio current. **Disable that block if you flash a
  stripped clone** that lacks the inductor — it would brown out otherwise.
- **LF clock forced to the internal RC oscillator** (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y`
  + `_500PPM` in the board `.conf`). The `promicro_nrf52840` board target defaults to an
  external **32.768 kHz crystal** (`K32SRC_XTAL`); the nice!nano v2 (and many Pro Micro /
  "SuperMini" clones) **has no 32 kHz crystal** and runs LFCLK off the internal RC (this is
  what ZMK does on the nice!nano too). RC needs no crystal (`RC_CALIBRATION` auto-enables to
  hold BLE timing within spec) and is safe even on a board that *does* populate the crystal
  — just slightly less power-efficient/accurate — so it's the right default for
  nice!nano-class boards. **Historical note:** this RC override was first added while
  chasing the "rapid LED flash" failure, on a *wrong* hypothesis (missing crystal). That
  symptom was actually the sysbuild/Partition-Manager `0x0` link bug above — the app never
  reached `bt_enable()`, so the LF clock was never even the issue. RC is retained because
  it is genuinely correct for the crystal-less nice!nano, not because it fixed the flash.
  Delete the two `K32SRC` lines if you confirm your board populates the 32 kHz crystal.

### nRF52840 Dongle (PCA10059, debug/bench — USB DFU, no debugger)
The dongle has **no on-board debugger**; it ships with the **Nordic Open USB bootloader**.
The board's Kconfig links the app at flash offset **`0x1000`** (after the factory MBR) via
`CONFIG_FLASH_LOAD_OFFSET=0x1000` (`BOARD_HAS_NRF5_BOOTLOADER=y && !USE_DT_CODE_PARTITION`,
no MCUboot) — verify it's `0x1000` in `build_dongle/nrfProxy/zephyr/.config` after building.
Note the dongle is the one board where the offset comes from `FLASH_LOAD_OFFSET`, *not* the
DT `code_partition`: its devicetree chooses the MCUboot `slot0` partition at `0x10000`, so
**do not enable `CONFIG_USE_DT_CODE_PARTITION`** here or the app would link at `0x10000` and
miss the Nordic USB bootloader. Disabling Partition Manager (below) leaves
`USE_DT_CODE_PARTITION` off, so the board Kconfig's `0x1000` wins — correct. This is a
**debug build** (logging + USB CDC-ACM console + `-Og`/thread-info via the board `.conf`):

```powershell
west build -b nrf52840dongle/nrf52840 --pristine `
  -d "$proj\build_dongle" "$proj" `
  -- -DSB_CONFIG_PARTITION_MANAGER=n
```

- **Flash by DFU, not `west flash`/J-Link.** Put the dongle in bootloader mode (press the
  small side RESET button — the red LED pulses), then either:
  - **nRF Connect for Desktop → Programmer** (easiest): select the dongle, add
    **`build_dongle/nrfProxy/zephyr/zephyr.hex`** (the app at 0x1000 — note the `nrfProxy/`
    sysbuild subdir), Write; or
  - **CLI:** package the hex into a DFU zip and flash it over the bootloader's USB serial
    (`nrfutil pkg generate ... --application zephyr.hex dfu.zip` then
    `nrfutil dfu usb-serial -pkg dfu.zip -p COMx`).
- Console/logs are over **USB CDC-ACM** (same log-visibility quirk + `CONFIG_UDC_BUF_POOL_SIZE`
  bump as the XIAO/Pro Micro). After the app boots it re-enumerates as a second CDC-ACM port.
- **LEDs:** the overlay drives the dongle's **RGB LED** (LD2) — green=connected (solid),
  blue=advertising (blink), red=error (solid). It **disables `pwm0` + the `pwmleds` node**
  (the board routes those RGB pins through PWM) so they can be plain GPIOs.
- **DC/DC** is already enabled by the board's own dts (the dongle has the inductor) — nothing
  to add in the overlay.
- **UART1** is on **P0.15 (TX) / P0.13 (RX)** — free, broken-out castellated pads. Cross the
  wires: device TX → board RX (P0.13), device RX ← board TX (P0.15); common ground, 115200.

### Production build (XIAO, no console/logging)
The default XIAO build keeps the USB CDC-ACM console + logging (handy for debugging — use
build dir `build_xiao`). For battery/production, layer `prod.conf` on top with
`EXTRA_CONF_FILE` into a **separate** build dir so the debug build is untouched:

```powershell
west build -b xiao_ble/nrf52840 --pristine -d "$proj\build_xiao_prod" `
  "$proj" -- -DSB_CONFIG_PARTITION_MANAGER=n -DEXTRA_CONF_FILE=prod.conf
```

`prod.conf` sets `CONFIG_LOG=n` and `CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n` (+ `CONSOLE`/
`UART_CONSOLE` off), which drops the USB device stack and console bring-up so the SoC can
idle between advertising events. UART1/NUS are unaffected (verify `CONFIG_UART_1_ASYNC=y`,
`CONFIG_SERIAL=y` still set). Flash the UF2 from `build_xiao_prod/nrfProxy/zephyr/zephyr.uf2`
(the XIAO links at `0x27000` from its DT `code_partition`, since its defconfig sets
`CONFIG_USE_DT_CODE_PARTITION=y`). There are **no logs** on this build — reflash the plain
`build_xiao` build to debug. Both `SB_CONFIG_PARTITION_MANAGER` and `EXTRA_CONF_FILE` are
per-command (remembered in CMakeCache for a non-pristine rebuild of that dir); a
`--pristine` rebuild must pass them again.

### The #1 gotcha: wrong SDK ⇒ `CONFIG_BT_NUS` undefined
`BT_NUS` / `bluetooth/services/nus.h` live in the **`nrf` module of nRF Connect SDK**,
not upstream Zephyr. If a build errors with *"attempt to assign value 'y' to undefined
symbol BT_NUS"* (or `nus.h: No such file`), the build is using a **plain Zephyr** tree
instead of NCS. If a separate standalone (plain-upstream) Zephyr workspace also exists on the
machine, never build against it. Symptoms: a `ZEPHYR_BASE` pointing at that plain-Zephyr tree
(not under `C:\ncs`) in `build_devkit/CMakeCache.txt`, and no `nrf` entry in
`build_devkit/zephyr_modules.txt`. Fix the IDE build config's **SDK = nRF Connect SDK
v3.3.1**, and on the CLI rebuild `--pristine` from the NCS workspace (`C:\ncs\v3.3.1`).
A non-pristine `-d build_devkit` reuses the cached (wrong) `ZEPHYR_BASE`.

### Per-instance UART async gating (`-ENOSYS`/-88 from uart_callback_set)
`uart1` is our async port. The nRF UARTE driver's per-instance `UART_<n>_ASYNC` depends on
`!UART_<n>_INTERRUPT_DRIVEN`, and `UART_<n>_INTERRUPT_DRIVEN` defaults `y` whenever anything
turns on `CONFIG_UART_INTERRUPT_DRIVEN` — which the **XIAO BLE's USB CDC-ACM console does**.
Result: on such boards uart1's async API is silently dropped and `uart_callback_set()`
returns `-ENOSYS` (-88) at runtime (the DK doesn't hit this). `prj.conf` pins this with
`CONFIG_UART_1_INTERRUPT_DRIVEN=n` (harmless on the DK). Any new async UART instance needs
the same `CONFIG_UART_<n>_INTERRUPT_DRIVEN=n`. Verify with `CONFIG_UART_1_ASYNC=y` in
`build*/<app>/zephyr/.config`.

### Version-specific API notes (NCS v3.3.1 / Zephyr 4.3)
- `BT_LE_ADV_CONN` was **removed**; use `BT_LE_ADV_CONN_FAST_2` (or `_FAST_1`).
- `k_mem_slab_free(slab, void *mem)` takes a `void *` (not `void **`).

## Architecture

Single file, two independent data paths decoupled by ring buffers so the UART byte rate
and BLE throughput don't have to match.

**Serial → phone:** UART1 async RX (double-buffered via `uart_rx_slab`) runs `uart_cb`
in **ISR context**. Each `UART_RX_RDY` chunk passes through the `on_uart_rx` hook, is
pushed to `uart_rx_ringbuf`, and signals `rx_data_ready`. `ble_write_thread` drains the
ring buffer and sends chunks (sized to the negotiated ATT MTU − 3) via `bt_nus_send`. On
`-ENOMEM`/`-EAGAIN` it keeps the data and backs off; otherwise consumes it. With no
connection, buffered data is discarded (intentional — data is repetitive/safe to drop).

**Phone → serial:** `nus_received` (Bluetooth RX thread) passes data through the
`on_ble_rx` hook into `uart_tx_ringbuf`, then `uart_tx_kick()`. Because `uart_tx()` allows
only one transfer in flight, `uart_tx_kick()` stages one contiguous chunk in `uart_tx_buf`
and the next chunk is started from the `UART_TX_DONE` event. The in-progress flag is
guarded with `irq_lock` so kicks from thread and ISR don't double-start.

**Interception hooks (`on_uart_rx`, `on_ble_rx`):** the intended place to inspect/modify/
filter data. Each copies `in`→`out` (scratch buffer, capacity `PROC_BUF_SIZE`) and returns
the output length; return 0 to drop. They can grow the data. **`on_uart_rx` runs in ISR
context — keep it light;** `on_ble_rx` runs in a thread.

**Connection lifetime:** `current_conn` is guarded by `conn_mutex`. `ble_write_thread`
takes its own `bt_conn_ref()` under the mutex before using the connection and unrefs after,
so a concurrent disconnect can't free the object mid-send (this was a real crash, triggered
by connect-then-immediately-disconnect from a phone's Bluetooth settings). Keep this
ref-counting discipline for any new code that touches `current_conn`.

**Advertising restart lives in the `recycled` callback, not `disconnected`.** Inside
`on_disconnected` the connection object is still allocated (the stack unrefs it after the
callbacks return), so with `CONFIG_BT_MAX_CONN=1` calling `bt_le_adv_start()` there fails
with `-ENOMEM` and the device is unreachable until reboot (real field bug: first connect
worked, every reconnect failed). `bt_conn_cb.recycled` fires once the object is back in
the pool — `on_recycled()` → `advertising_start()` covers both clean disconnects and
failed connection attempts (the `err != 0` path in `on_connected` deliberately does *not*
restart advertising for the same reason). Don't move the restart back into
`on_disconnected`.

**…and `recycled` also fires on `bt_le_adv_stop()`, hence the `adv_active` flag.** Legacy
connectable advertising *pre-allocates* a connection object, so stopping the advertiser
(the fast→slow interval switch in `adv_slow_handler`) frees that object and fires
`recycled` too — a second field bug: `on_recycled` then re-called `advertising_start()`
against the already-running slow advertiser → `-EALREADY` (err -120) → spurious error LED
(and, had the race gone the other way, a fast advertiser preempting the slow switch).
`adv_active` (guarded by `conn_mutex`, same as `current_conn`) tracks whether an
advertiser is live; `advertising_start()` no-ops when connected or already advertising,
and `adv_slow_handler` holds the mutex with `adv_active` still true across its
stop→start gap so a concurrent `recycled` can't sneak in. `on_connected` clears
`adv_active` in both branches (the connection attempt consumed the advertiser either
way).

## Conventions / gotchas

- **Git is the user's job — but remind, don't run.** The user handles all git
  themselves (commits, branches, pushes); do not run git write commands. It *is* helpful
  to remind them to **commit** at a good stopping point, and to **create a new branch** when
  we switch to a different topic/task. Surface the suggestion; let them do it.
- Code style is Zephyr/Linux kernel: tabs, `LOG_*` for output, K_*_DEFINE static objects.
- **Per-board Kconfig:** `prj.conf` holds the shared config; board-specific deltas go in
  `boards/<board>.conf`, which Zephyr auto-merges on top (same naming as the overlays).
  `nrf52840dk_nrf52840.conf` enables `CONFIG_DEBUG_OPTIMIZATIONS=y` (-Og) +
  `CONFIG_DEBUG_THREAD_INFO=y` for debugging; `xiao_ble_nrf52840.conf` (production) uses
  `CONFIG_SIZE_OPTIMIZATIONS=y`. Put board-only settings here, not in `prj.conf`.
- **Production target is the XIAO BLE** (`xiao_ble/nrf52840`); the DK is just for
  bench/debug. No flash readback / `APPROTECT` or other security hardening is wanted — the
  firmware is being open-sourced. Don't add protection-oriented config unasked.
- **Per-device identity** (`identity_init()` in `main.c`): without this, Zephyr brings up
  the controller with a random-static address **regenerated from the RNG each boot**, and
  every unit advertises the same `CONFIG_BT_DEVICE_NAME` — so units are indistinguishable
  and the address is useless as an ID. `identity_init()` reads the SoC's unique hardware ID
  (`hwinfo_get_device_id()`, needs `CONFIG_HWINFO=y`) and derives **(a)** a fixed
  static-random BT address from the low 6 ID bytes (`BT_ADDR_SET_STATIC` + `bt_id_create()`,
  **must run before `bt_enable()`**) and **(b)** a `nrfProxy-XXXX` name suffix from the top
  ID bytes. The address is stable across reboots *without* `CONFIG_BT_SETTINGS`/flash —
  it's recomputed deterministically each boot. The name is applied after `bt_enable()` via
  `bt_set_name()` (needs `CONFIG_BT_DEVICE_NAME_DYNAMIC=y`, `CONFIG_BT_DEVICE_NAME_MAX`) and
  pushed into `ad[1].data_len`; `ad[]` is therefore non-const. Falls back to the
  compile-time name/random address if the hardware ID can't be read.
- **Status LEDs** are chosen by role, not by fixed alias: `main.c` reads the
  `led-connected` / `led-advertising` / `led-error` aliases, which **each board overlay
  defines** (XIAO: green/blue/red = led1/led2/led0; DK: led1/led2/led3; Dongle RGB:
  green/blue/red; Pro Micro: single LED shared by connected+advertising). They use
  `GPIO_DT_SPEC_GET_OR`, so a board that omits a role silently gets a **dark LED** — usually
  define all three, but omitting one is a valid way to disable it (the Pro Micro deliberately
  leaves `led-error` unmapped so errors light nothing on its single LED). Connected/error are
  **solid**; advertising **blinks at a low
  duty cycle** (`adv_blink_handler`, ~30 ms on) — a solid LED would dominate the battery
  budget next to sub-100uA advertising. The off-gap is **phase-aware**: brisk
  (`ADV_BLINK_OFF_FAST`, ~300 ms) during the fast-advertising phase and lazy
  (`ADV_BLINK_OFF_SLOW`, ~2 s) once the switch to slow advertising happens, via the
  `adv_slow_phase` flag (set false in `advertising_start`, true in `adv_slow_handler`). So
  the fast→slow transition is **visible on the LED** even when the console isn't — useful on
  the XIAO, whose USB log is unreliable (see below). `set_status_leds()` drives the blink via
  `adv_blink_work` and tracks state in `current_status` so the work self-stops on leaving
  the advertising state.
- **Power: two-phase advertising.** `advertising_start()` advertises at the fast interval
  (`BT_LE_ADV_CONN_FAST_2`, ~100 ms) for quick discovery, then `adv_slow_work` switches to
  a slow ~1 s interval (`adv_param_slow`, `BT_GAP_ADV_SLOW_INT_*`) after
  `ADV_FAST_DURATION` (30 s) to cut standby radio current. The switch does
  `bt_le_adv_stop()` + `bt_le_adv_start()` (interval can't be changed in place). On connect,
  `on_connected` **cancels** `adv_slow_work` (advertising already stopped); the handler also
  re-checks `current_conn` to guard the connect-vs-timer race. **Failed advertising starts
  self-heal**: every failure path schedules `adv_retry_work` (1 s), which re-runs
  `advertising_start()` — the error LED shows only until a retry succeeds. Stale retries are
  no-ops via the `current_conn`/`adv_active` guard. Boot-time init failures (`uart_init`,
  `bt_enable`, `bt_nus_init`) stay terminal on purpose (hardware/config faults). The USB CDC-ACM console +
  logging are dropped in the production build via `prod.conf` (see "Production build") so the
  SoC can idle between adv events. The XIAO overlay enables the main **DC/DC regulator**
  (`&reg1` mode `NRF5X_REG_MODE_DCDC`) for lower active/radio current — it has the required
  DCC/DEC4 inductor populated and VDDH tied to VDD (normal-voltage mode, so REG0 is unused).
  A board lacking that inductor must **not** set this (it would brown out); it's per-board,
  hence in the overlay, not `prj.conf`.
- UART1 pins are per-board (in each overlay); default baud **115200** (`current-speed`).
  Common ground with the serial source is required.
  - **nRF52840 DK:** RX = P1.01, TX = P1.02. Console/logs on the J-Link VCOM (uart0).
  - **XIAO BLE:** RX = D1/P0.03, TX = D0/P0.02 (UARTE1, independent of uart0/i2c1/spi2).
    No debug UART — console/logs come out the **USB CDC-ACM** serial port (board default).
    Its `boards/xiao_ble_nrf52840.conf` bumps `CONFIG_UDC_BUF_POOL_SIZE` (default 1024 is
    too small for the control transfers a **Windows** host makes at enumeration → `udc:
    Failed to allocate net_buf …, ep 0x80`; Zephyr issue #85108).
    **USB log visibility quirk:** the XIAO console only exists while USB is enumerated, and
    during boot the bus resets/re-enumerates (`udc_nrf: Reset` / `SUSPEND` / `RESUMING`),
    which on Windows changes the COM port and **drops the terminal session** — so you often
    capture only the first ~0.2 s of logs and *nothing after*. Later lines (e.g.
    `Advertising (slow)` at 30 s) are still emitted; the terminal just isn't attached.
    To see them, (re)open the serial port a couple seconds *after* boot/enumeration settles
    and keep it open — or rely on the phase-aware advertising LED, which doesn't need the
    console. The DK (J-Link VCOM) has none of this. Don't chase a "missing log" on the XIAO
    as a firmware bug before ruling this out.
- Hooks/forwarding operate on transport **chunks**, not framed application messages
  (UART = whatever DMA delivered per idle-timeout/buffer; BLE = one GATT write). Add
  reassembly if framed messages are needed.

## Status / next steps (as of 2026-06-29)

The bidirectional bridge is feature-complete and **compile-verified for all four boards**
(DK, XIAO BLE, Pro Micro nRF52840 / nice!nano, nRF52840 Dongle — hardware flashing/testing
is done by the user, not in-session). Done so far: UART1⇄BLE NUS bridging both directions;
per-board overlays + `.conf` fragments (DK debug / XIAO production / Pro Micro UF2+DC-DC /
Dongle USB-DFU debug); role-based status LEDs; per-device identity (stable static-random address +
`nrfProxy-XXXX` name + manufacturer-data tag from the chip's hardware ID, see Conventions);
power tuning (two-phase fast→slow advertising + phase-aware advertising LED blink, XIAO DC/DC
regulator, and a logging/USB-free `prod.conf` production build); and the two board-specific
runtime bugs above (uart1 async `-ENOSYS`, USB `net_buf` pool) fixed and documented.

All builds were **migrated off the (deprecated) Partition Manager to DTS partitioning** via
`-DSB_CONFIG_PARTITION_MANAGER=n` (sysbuild kept). Re-verified after the migration that no
build emits `partitions.yml` and each links at its correct DT/Kconfig offset (DK `0x0`,
XIAO `0x27000`, Pro Micro `0x26000`, Dongle `0x1000`); the Pro Micro's old `--no-sysbuild`
special case was removed since disabling PM fixes the same `0x0` link bug for all boards.

Open threads / likely next work:
- **Interception hooks are pass-through stubs.** `on_uart_rx` / `on_ble_rx` just copy
  input→output; the real inspect/modify/filter logic is still to be written (this is the
  user's main extension point).
- **No message framing.** Hooks see transport chunks, not delimited/framed messages; add
  reassembly (e.g. newline-terminated) if the data is message-oriented.
- A second **production board on a different SoC** was once floated but is **not** current —
  the XIAO BLE is the production target. Revisit only if the user reopens it.
- Git is managed by the user; if starting fresh, check `git log`/`git status` to see which
  of the above has been committed.
