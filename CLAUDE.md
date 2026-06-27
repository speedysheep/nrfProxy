# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`nrfProxy` is a Zephyr / nRF Connect SDK application that bridges a serial UART to a
phone over Bluetooth LE, acting as a **bidirectional proxy**:

- **Serial → phone:** bytes received on UART1 are forwarded as BLE notifications via the
  Nordic UART Service (NUS).
- **Phone → serial:** bytes written to the NUS RX characteristic are sent out UART1.

Target board: **nRF52840 DK** (`nrf52840dk/nrf52840`). UART0 stays as the J-Link VCOM
console/logging; UART1 is the data channel. Connect/test with the *nRF Connect for
Mobile* or *nRF Toolbox* apps.

## Files

- `src/main.c` — entire application (UART async driver, BLE/NUS, the bridge logic).
- `prj.conf` — Kconfig: BLE peripheral + NUS, async UART, MTU/throughput tuning, debug flags.
- `boards/nrf52840dk_nrf52840.overlay` — enables UART1 and assigns its pins.
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

- Code style is Zephyr/Linux kernel: tabs, `LOG_*` for output, K_*_DEFINE static objects.
- `prj.conf` currently enables `CONFIG_DEBUG_OPTIMIZATIONS=y` (-Og) and
  `CONFIG_DEBUG_THREAD_INFO=y` so VS Code's debugger can place breakpoints / enumerate
  threads. Remove these for a size/speed-optimized production build.
- A **production board target** (different SoC) is planned but not yet defined; the current
  overlay and any nRF52840-specific pin choices will need a board-specific overlay then.
- UART1 pins (overlay): **RX = P1.01**, **TX = P1.02**, default baud **115200**
  (`current-speed` in the overlay). P1.01/P1.02 are free of the DK's LEDs/buttons/QSPI/NFC.
  Common ground with the serial source is required.
- Hooks/forwarding operate on transport **chunks**, not framed application messages
  (UART = whatever DMA delivered per idle-timeout/buffer; BLE = one GATT write). Add
  reassembly if framed messages are needed.
```
