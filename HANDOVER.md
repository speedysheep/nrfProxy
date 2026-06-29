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
  - `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` + `_500PPM` (added 2026-06-29). Forces the
    internal RC oscillator for LFCLK instead of the board target's default 32.768 kHz
    crystal — correct for the crystal-less nice!nano. NB: added while chasing the
    "rapid-flash" bug on a wrong hypothesis; it was *not* the fix (see follow-up below) but
    is kept because it's genuinely right for this board.

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
# --no-sysbuild is MANDATORY here (see "rapid flash" section below) — without it
# Partition Manager links the app at 0x0 and the board reboot-loops.
west build -b promicro_nrf52840/nrf52840/uf2 --pristine --no-sysbuild `
  -d "C:\Users\extra\projects\nrfProxy\build_promicro" "C:\Users\extra\projects\nrfProxy"
```
- Flash by **double-tap RESET** → the `NICENANO` USB mass-storage drive appears → copy
  **`build_promicro/zephyr/zephyr.uf2`** onto it. With `--no-sysbuild` there is **no
  `nrfProxy/` image subdir** (that path is the sysbuild output and would be the
  broken-at-0x0 image). Do **not** use `west flash`.
- Console/logs are over USB CDC-ACM (same XIAO re-enumeration log-drop quirk — reopen the
  serial port a couple seconds after boot). For battery use, layer `prod.conf` via
  `EXTRA_CONF_FILE` exactly like the XIAO.

## Verification status
**HARDWARE-VERIFIED 2026-06-29** on a genuine nice!nano (Board-ID `nRF52840-nicenano`,
UF2 bootloader 0.6.0). With the `--no-sysbuild` build (below), both a stock Zephyr blinky
and the nrfProxy app run correctly — the app advertises and the UART1⇄BLE bridge works.
(One spare unit is a **dead board**: flashing it changes nothing — faulty bootloader/flash,
not a firmware issue.)

## "Rapid green LED flash" — 2026-06-29, REAL root cause (build system, not hardware)
A board flashed with the original (sysbuild) build "did not work at all — just rapid green
LED flash"; the app never ran and no USB CDC port enumerated.

**Initial wrong hypothesis (missing 32 kHz crystal).** We first suspected `K32SRC_XTAL`
with no crystal and added `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` + `_500PPM`. It made **no
difference** — because the app never reached `bt_enable()`, so the LF clock was never the
problem. (RC is kept anyway: it's genuinely correct for the crystal-less nice!nano. Not the
fix.) Disabling DC/DC also made no difference, for the same reason.

**Actual root cause: NCS sysbuild / Partition Manager linked the app at `0x0`.**
`promicro_nrf52840` is a community `others/` board target with a plain-Zephyr DT
`code_partition` at `0x26000` but **no Partition Manager metadata**. NCS's default
`west build` runs sysbuild → PM, and with no PM layout for this board PM put the `app`
partition at **`0x0` (full flash)** (`build_promicro/partitions.yml`). The image was thus
*linked to run from 0x0* while the UF2 step copied it to `0x26000` → reset vector pointed
into low flash → HardFault on every boot → reboot loop / rapid flash, no app, no USB port.
Diagnosed by reading the bootloader's `CURRENT.UF2`: the vector table at `0x26000` had
reset vector `0x00000bd1` (base-0) vs a fresh board's factory app at `0x0003847d` (in
region).

**Fix: build `--no-sysbuild`** → bypasses PM, uses the DT `code_partition`
(`CONFIG_USE_DT_CODE_PARTITION=y`) → app correctly linked at `0x26000` (reset vector now
`0x000354xx`, in region). Confirmed on hardware. This is **Pro-Micro-specific**: the XIAO
and Dongle ship proper PM metadata (XIAO: `softdevice_reserved` 0x0–0x27000 + app 0x27000;
Dongle: `nrf5_mbr` 0x0–0x1000 + app 0x1000) and build correctly *with* sysbuild — leave
them alone. The DK has no bootloader (app at 0x0) so PM's default is fine there.

## Next steps
1. ✅ Done — hardware-verified (blinky + nrfProxy run, advertises, bridges) on the good unit.
2. **Commit.** Uncommitted on `main`: Pro Micro overlay/`.conf` (incl. RC clock + DC/DC),
   the `--no-sysbuild` doc fixes in `CLAUDE.md`/`HANDOVER.md`, and the nRF52840 Dongle board
   added in the same session. Git is the user's job — remind, don't run.

## Related
- Same session also added the **nRF52840 Dongle** debug board
  (`boards/nrf52840dongle_nrf52840.{overlay,conf}`) — see the "nRF52840 Dongle" section in
  `CLAUDE.md`.
- Full per-board details live in `CLAUDE.md` (board list, files, build sections, LED/power
  conventions).
