# Handover — Pro Micro nRF52840 / nice!nano board support

_Session date: 2026-06-29. Status: compile-verified, hardware flash/test by user still pending._

## What it is
A user-supplied nRF52840 board in Pro Micro form factor with a nice!nano bootloader.
Confirmed a genuine nice!nano via `INFO_UF2.TXT`: `Board-ID: nRF52840-nicenano`, Adafruit
UF2 Bootloader 0.6.0, `SoftDevice: not found` (expected/harmless — Zephyr brings its own
BLE controller, no Nordic SoftDevice needed). It was "not working" because these boards
have **no debugger** and must be flashed via the UF2 bootloader at the right flash offset —
not `west flash`/J-Link.

## Board target
`promicro_nrf52840/nrf52840/uf2` — already defined in NCS v3.3.1's Zephyr tree
(`zephyr/boards/others/promicro_nrf52840`). The `/uf2` variant links the app at flash
offset **`0x26000`** (SoftDevice s140 v6 layout) to match the bootloader.

## Files added
- **`boards/promicro_nrf52840_nrf52840_uf2.overlay`**
  - UART1 (data channel): **TX = P0.06, RX = P0.08** @ 115200 (the nice!nano's silkscreen
    TX/RX pads). Cross the wires: device TX → board RX (P0.08), device RX ← board TX (P0.06);
    common ground.
  - LEDs: single LED (`led0` = P0.15, **active-high**). `led-connected` and `led-advertising`
    both → `led0` (blink while advertising, solid when connected; states are mutually
    exclusive so it's unambiguous). **`led-error` deliberately unmapped** → no LED on error
    (explicit user request; `main.c`'s `GPIO_DT_SPEC_GET_OR` null-fallback handles the
    missing alias).
  - **DC/DC regulator ENABLED** (`&reg1` mode `NRF5X_REG_MODE_DCDC`) — genuine nice!nano has
    the DCC/DEC4 inductor. User accepted the brownout risk ("I have spare boards"). Disable
    this block if flashing a stripped clone that lacks the inductor.
- **`boards/promicro_nrf52840_nrf52840_uf2.conf`**
  - `CONFIG_UDC_BUF_POOL_SIZE=8192` (Windows USB CDC-ACM enumeration fix, same as XIAO).
  - `CONFIG_UART_1_INTERRUPT_DRIVEN=n` is global (in `prj.conf`) and applies here too — keeps
    uart1's async API from being gated by the USB CDC-ACM console.

## Critical gotcha (cost a failed build)
The board-file filename **must include the `uf2` variant**:
`promicro_nrf52840_nrf52840_uf2.{overlay,conf}`. The first attempt named them
`promicro_nrf52840_nrf52840.*` (matching the XIAO's no-variant pattern) → **silently
ignored**, uart1 stayed disabled, and the build failed at `DEVICE_DT_GET`. The full
normalized board target (`/`→`_`, **including the variant**) is required. The XIAO/DK have
no variant, hence no suffix.

## Build & flash
```powershell
# (set the NCS env block from CLAUDE.md first)
west build -b promicro_nrf52840/nrf52840/uf2 --pristine `
  -d "C:\Users\extra\projects\nrfProxy\build_promicro" "C:\Users\extra\projects\nrfProxy"
```
- Flash by **double-tap RESET** → the `NICENANO` USB mass-storage drive appears → copy
  **`build_promicro/nrfProxy/zephyr/zephyr.uf2`** onto it (note the `nrfProxy/` sysbuild
  image subdir — **not** `build_promicro/zephyr/`). Do **not** use `west flash`.
- Console/logs are over USB CDC-ACM (same XIAO re-enumeration log-drop quirk — reopen the
  serial port a couple seconds after boot). For battery use, layer `prod.conf` via
  `EXTRA_CONF_FILE` exactly like the XIAO.

## Verification status
**Compile-verified only** (final DC/DC build, exit 0). Confirmed in the generated
config/dts: `CONFIG_UART_1_ASYNC=y`, `CONFIG_BT_NUS=y`, `CONFIG_UDC_BUF_POOL_SIZE=8192`,
`CONFIG_BUILD_OUTPUT_UF2=y`, `uart1` status okay @115200, `led-connected`/`led-advertising`
→ `led0`, `led-error` absent, REG1 `regulator-initial-mode = <0x1>` (DC/DC), and
`zephyr.uf2` produced. **Hardware flash/test by the user is still pending.**

## Next steps
1. User flashes `zephyr.uf2` and confirms: advertises as `nrfProxy-XXXX`, LED blinks while
   advertising / solid on connect / dark on error, and UART1⇄BLE bridging works both ways
   (wire a 3.3 V serial source to P0.08 RX / P0.06 TX, 115200, common ground).
2. If the board browns out (no enumeration, no advertising LED) the DC/DC is the suspect —
   disable the `&reg1` block in the overlay and rebuild, or use a spare board.
3. **Not yet committed** — these changes (Pro Micro overlay/conf + CLAUDE.md, plus the
   nRF52840 Dongle board added in the same session) are uncommitted on `main`. Git is the
   user's job.

## Related
- Same session also added the **nRF52840 Dongle** debug board
  (`boards/nrf52840dongle_nrf52840.{overlay,conf}`) — see the "nRF52840 Dongle" section in
  `CLAUDE.md`.
- Full per-board details live in `CLAUDE.md` (board list, files, build sections, LED/power
  conventions).
