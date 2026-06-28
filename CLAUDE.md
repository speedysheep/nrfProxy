# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`nrfProxy` is a Zephyr / nRF Connect SDK application that bridges a serial UART to a
phone over Bluetooth LE, acting as a **bidirectional proxy**:

- **Serial → phone:** bytes received on UART1 are forwarded as BLE notifications via the
  Nordic UART Service (NUS).
- **Phone → serial:** bytes written to the NUS RX characteristic are sent out UART1.

Supported boards (each has its own overlay in `boards/`): **nRF52840 DK**
(`nrf52840dk/nrf52840`) and **Seeed XIAO BLE** (`xiao_ble/nrf52840`). UART1 is the data
channel on both; `main.c` is board-agnostic (references `DT_NODELABEL(uart1)`). Connect/
test with the *nRF Connect for Mobile* or *nRF Toolbox* apps.

## Files

- `src/main.c` — entire application (UART async driver, BLE/NUS, the bridge logic).
- `prj.conf` — Kconfig: BLE peripheral + NUS, async UART, MTU/throughput tuning, debug flags.
- `boards/<board>_<qualifier>.overlay` — per-board: enables UART1 and assigns its pins
  (`nrf52840dk_nrf52840.overlay`, `xiao_ble_nrf52840.overlay`). Add one per new board.
- `CMakeLists.txt` — standard Zephyr app boilerplate; only `src/main.c` is compiled.
- `.mcp.json` — Memfault MCP server config (unrelated to firmware).

There is no README, no test suite, and no Cursor/Copilot rules.

## Build environment (IMPORTANT — read before building)

The nRF Connect SDK is installed at `C:\ncs\v3.3.1` (Zephyr 4.3.99) with toolchain
`936afb6332`. `west`, `cmake`, etc. are **not on PATH** by default. Set the environment,
then build **from inside the NCS workspace** so the `nrf` module is in scope:

```powershell
$tc = "C:\ncs\toolchains\936afb6332"
$env:PATH = "$tc;$tc\mingw64\bin;$tc\bin;$tc\opt\bin;$tc\opt\bin\Scripts;$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;$tc\opt\zephyr-sdk\arm-zephyr-eabi\bin;$tc\opt\zephyr-sdk\riscv64-zephyr-elf\bin;$env:PATH"
$env:PYTHONPATH = "$tc\opt\bin;$tc\opt\bin\Lib;$tc\opt\bin\Lib\site-packages"
$env:NRFUTIL_HOME = "$tc\nrfutil\home"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
Set-Location "C:\ncs\v3.3.1"
west build -b nrf52840dk/nrf52840 -d "C:\Users\extra\projects\nrfProxy\build" "C:\Users\extra\projects\nrfProxy"
```

- Add `--pristine` after a config/overlay change or to clear a bad cache.
- Incremental rebuild (after editing `src/main.c` only): drop `-b` and `--pristine`,
  keep `-d <build>`.
- Flash: `west flash -d build`, or drag `build/merged.hex` onto the J-Link drive.
- First full build takes a few minutes (BLE stack); run it in the background.

### The #1 gotcha: wrong SDK ⇒ `CONFIG_BT_NUS` undefined
`BT_NUS` / `bluetooth/services/nus.h` live in the **`nrf` module of nRF Connect SDK**,
not upstream Zephyr. If a build errors with *"attempt to assign value 'y' to undefined
symbol BT_NUS"* (or `nus.h: No such file`), the build is using a **plain Zephyr** tree
instead of NCS. There is a standalone Zephyr workspace at `C:\Users\extra\zephyr` — never
build against it. Symptoms: `ZEPHYR_BASE=C:/Users/extra/zephyr` in `build/CMakeCache.txt`,
and no `nrf` entry in `build/zephyr_modules.txt`. Fix the IDE build config's **SDK =
nRF Connect SDK v3.3.1**, and on the CLI rebuild `--pristine` from `C:\ncs\v3.3.1`.
A non-pristine `-d build` reuses the cached (wrong) `ZEPHYR_BASE`.

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
  must define** (XIAO: green/blue/red = led1/led2/led0; DK: led1/led2/led3). They use
  `GPIO_DT_SPEC_GET_OR`, so a board that omits a role silently gets a dark LED — define all
  three when adding a board.
- UART1 pins are per-board (in each overlay); default baud **115200** (`current-speed`).
  Common ground with the serial source is required.
  - **nRF52840 DK:** RX = P1.01, TX = P1.02. Console/logs on the J-Link VCOM (uart0).
  - **XIAO BLE:** RX = D1/P0.03, TX = D0/P0.02 (UARTE1, independent of uart0/i2c1/spi2).
    No debug UART — console/logs come out the **USB CDC-ACM** serial port (board default).
    Its `boards/xiao_ble_nrf52840.conf` bumps `CONFIG_UDC_BUF_POOL_SIZE` (default 1024 is
    too small for the control transfers a **Windows** host makes at enumeration → `udc:
    Failed to allocate net_buf …, ep 0x80`; Zephyr issue #85108).
- Hooks/forwarding operate on transport **chunks**, not framed application messages
  (UART = whatever DMA delivered per idle-timeout/buffer; BLE = one GATT write). Add
  reassembly if framed messages are needed.

## Status / next steps (as of 2026-06-28)

The bidirectional bridge is feature-complete and **compile-verified for both boards**
(hardware flashing/testing is done by the user, not in-session). Done so far: UART1⇄BLE
NUS bridging both directions; per-board overlays + `.conf` fragments (DK debug / XIAO
production); role-based status LEDs; per-device identity (stable static-random address +
`nrfProxy-XXXX` name from the chip's hardware ID, see Conventions); and the two
board-specific runtime bugs above (uart1 async `-ENOSYS`, USB `net_buf` pool) fixed and
documented.

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
