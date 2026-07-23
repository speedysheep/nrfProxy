# nrfProxy — Code Review Findings & Test Plan

Review date: 2026-07-02 (post advertising-restart/`recycled` fixes, pre-commit).
Scope: whole project — `src/main.c`, `prj.conf`, `prod.conf`, `boards/*`, build scripts, docs.
No code was changed as part of this review; every item below is a TODO.

Severity scale: **High** = real defect likely to bite in normal use · **Medium** = defect or
gap that bites under specific conditions · **Low** = minor/cosmetic/robustness ·
**Info** = awareness item, no action strictly required.

---

## Issues (ranked by severity)

### M2. UART RX can stop permanently if buffer recovery fails — ✅ FIXED
- Was: `UART_RX_DISABLED` gave up if `k_mem_slab_alloc()` / `uart_rx_enable()` failed
  in the ISR, so serial→BLE died silently.
- **Fixed:** inline restart kept; on failure `uart_rx_retry_work` retries every 100 ms
  (`-EBUSY` = success). Policy helpers in `src/uart_rx_retry.c` (host-tested).

### M3. Unauthenticated, unencrypted NUS — ✅ ADDRESSED (pairing lock implemented)
- Was: the NUS RX characteristic accepted writes from any connected central; no pairing,
  bonding, or encryption. Whoever connected first owned the bridge — and the serial peer
  is an e-bike motor controller (assist level, walk mode, etc.).
- **Implemented per `PAIRING_PLAN.md`:** bond-to-first-phone (SMP Just Works +
  `CONFIG_BT_SETTINGS`), filter-accept-list advertising in locked mode, an app-layer
  encryption gate (`link_secure`) + 10 s security watchdog, and a per-board `bond-reset`
  button (hold at boot 3 s → `bt_unpair`). Compile-verified on all six configs; **not yet
  hardware-tested** — see `PAIRING_PLAN.md §6`. The bafflingvision app still needs the
  matching bonding/UI work in `PAIRING_PLAN.md §7`.

### L1. Silent data loss points — no counters, no logs — ✅ FIXED
- Was: `uart_rx_ringbuf` overflow, `uart_tx()` start failure, and `UART_TX_ABORTED`
  remainder all dropped bytes with no trace.
- **Fixed:** ISR-safe `atomic_t` counters + periodic `LOG_WRN` from `ble_write_thread`
  (cumulative totals, ≥10 s apart when changed). Disconnect drain is intentional and
  not counted. Helpers in `src/drop_stats.c` (host-tested).

### L2. Transient wrong LED / error log if a central connects exactly at the fast→slow switch — ✅ FIXED
- Was: `adv_slow_handler()` can run in the window where the controller has already accepted a
  connection but `on_connected` hasn't updated state; its `bt_le_adv_start(slow)` then fails
  (no free conn object) → error LED + log, corrected a moment later by `on_connected`.
  Harmless but confusing during debugging.
- **Fixed:** the slow-start failure path re-checks `current_conn`. If a connection arrived it
  logs at INFO and returns (`on_connected` owns the LED/adv state) — no spurious error LED.
  Otherwise it retries via `adv_retry_work` **without** lighting `STATUS_ERROR`;
  `advertising_start()` surfaces `STATUS_ERROR` only if the retry genuinely can't advertise, so
  a transient race never shows red while a real, persistent failure still does.

### L3. LED state machine has benign races and a cosmetic blink quirk
- `set_status_leds()` / `current_status` are touched from main, BT host thread, and the
  system workqueue with no synchronization; interleaved calls could briefly light two LEDs.
- `adv_blink_handler`'s `static bool on` (`src/main.c:129`) carries over between advertising
  sessions, so the first blink after re-entering advertising can start with an off-gap.
- Cosmetic only; fold into any future LED rework.

### L4. README nits
- ✅ FIXED: "Per-board Kconfig fragments" table labeled the XIAO and Pro Micro rows "(USB DFU)"
  — both flash by UF2 drag-and-drop (only the Dongle is USB DFU, via nrfutil/Programmer). The
  main board table already had it right; the fragment table now says "(UF2 drag-and-drop)".
- README calls the plain `xiao` target a "debug build" while
  `boards/xiao_ble_nrf52840.conf`'s header comment calls the XIAO "the production board".
  Both are true (debug *build* of the production *board*) but the wording invites confusion.

### I1. Throughput depends on the peer negotiating a larger ATT MTU
- The firmware advertises support for 247-byte MTU / 251-byte DLE, but as a peripheral it
  never initiates the exchange. A central that stays at the default 23-byte MTU gets
  20-byte notifications; a sustained 115200 stream may then outrun the link at long
  connection intervals. The bafflingvision app / nRF Connect do negotiate — just know the
  firmware can't force it.

### I2. Interception hooks run in ISR context (serial→phone side)
- `on_uart_rx` (`src/main.c:70-78`) is called from the UART ISR. Fine as a memcpy stub, but
  the planned inspect/modify/filter logic (framing, checksums) doesn't belong in an ISR. When
  the hooks grow, move the serial-side hook into `ble_write_thread` (process after the ring
  buffer, not before). Also note `irq_lock()` in `uart_tx_kick()` holds interrupts across a
  ≤64-byte copy — fine now, worth keeping small.

### I3. Identity details (no action)
- The advertised company ID is `0xFFFF` (SIG test ID) — already documented as "replace
  before shipping".
- The `-XXXX` name suffix is 16 bits of the hardware ID: two units can collide (1-in-65536);
  the BT address and 4-byte manufacturer-data ID stay unique, so scanners filtering properly
  are unaffected.

---

## Test plan (currently ~zero tests; build these next)

Recommended infrastructure first: a `tests/` directory with **ztest** suites run via
**Twister**, targeting `native_sim` where possible. The biggest enabler is a small refactor
(deliberately NOT done in this review): extract the pure logic out of `src/main.c` into a
unit-testable core (e.g. `proxy_core.c`) — the hooks, identity derivation, and state-machine
decisions — leaving `main.c` as glue. Items marked ⚙ need that refactor or a mocking layer;
items marked ▶ run against real APIs on `native_sim`; items marked 🔩 need hardware or
BabbleSim.

### Unit tests
1. **Interception hooks** (`on_uart_rx`, `on_ble_rx`) ⚙ — the project's main extension
   point, test-first before real filter logic lands:
   - output clamped to `out_size`; return-0 drops; growth up to `PROC_BUF_SIZE`;
   - zero-length input; input exactly `PROC_BUF_SIZE`.
2. **Identity derivation** (`identity_init`, `src/main.c:236-269`) ⚙:
   - fixed hwid → expected static-random address (two MSBs forced by `BT_ADDR_SET_STATIC`);
   - name suffix formatting (`nrfProxy-3F7A` from hwid[5],[4]); buffer bounds (no overflow of
     `device_name`);
   - manufacturer-data layout: company ID little-endian, `'N','P'` tag, hwid[0..3] at [4..7];
   - fallback path when `hwinfo_get_device_id` returns <6 bytes (default name kept, no
     address created).
3. **Advertising/connection state machine** (`advertising_start`, `adv_slow_handler`,
   `on_connected`, `on_disconnected`, `on_recycled`, `adv_active`) ⚙ — this just produced two
   real field bugs; regression-protect the event sequences:
   - disconnect → `recycled` → advertising restarts (the `-ENOMEM`-in-`disconnected` bug);
   - fast→slow switch fires `recycled` → no second advertiser, no `-EALREADY` (the err -120 bug);
   - failed connection attempt (`on_connected` with err) → `recycled` restarts advertising;
   - connect during fast phase → slow-switch work cancelled, `adv_active` false;
   - `on_recycled` while already advertising or connected → no-op;
   - invariant: `adv_active` ⇒ not connected; never two advertisers.
4. **UART TX staging** (`uart_tx_kick`, TX_DONE chaining) ▶/⚙:
   - >64-byte queue split into sequential `uart_tx()` calls, chained from `UART_TX_DONE`;
   - concurrent kicks (thread + ISR) start exactly one transfer;
   - `uart_tx()` failure: flag cleared, next kick recovers (and, once fixed per L1, the drop
     is reported).
5. **BLE writer thread flow control** (`ble_write_thread`, `src/main.c:573-627`) ⚙:
   - chunks sized to MTU−3; MTU 23 → 20-byte chunks;
   - `-ENOMEM`/`-EAGAIN` keeps data and retries; other errors drop the chunk;
   - no connection → buffer discarded; wrap-around claims (two claims per physical wrap).
6. **LED state machine** (`set_status_leds`, `adv_blink_handler`) ▶:
   - transitions light exactly one role; boards with missing aliases no-op (Pro Micro error);
   - blink off-gap switches fast→slow with `adv_slow_phase`;
   - leaving advertising stops the blink work.

### Integration tests
7. **UART loopback on `native_sim`** (uart_emul) ▶: serial in → hook → ring buffer → (mock
   NUS) and NUS write → ring buffer → serial out; includes overflow/drop accounting (H1/L1
   regression: disconnect flush while producer active).
8. **End-to-end BLE with BabbleSim** 🔩: simulated central connects, subscribes, and
   round-trips data; disconnect/reconnect cycles (regression for the reconnect bug);
   fast→slow advertising observable via advertising intervals.
9. **Sustained-throughput soak** 🔩 (bsim or hardware-in-loop): continuous 115200 stream for
   minutes; assert zero drops while connected with a healthy link; measure ring-buffer high
   water mark.

### Build/config regression (cheap, high value — this project's history is config gotchas)
10. **Twister build-only matrix** for all four board targets, asserting per-board
    `CONFIG_FLASH_LOAD_OFFSET` (DK `0x0`, XIAO `0x27000`, Pro Micro `0x26000`, Dongle
    `0x1000`), `CONFIG_UART_1_ASYNC=y`, and absence of `partitions.yml` — regression-protects
    the Partition-Manager/0x0-link and `-ENOSYS` uart traps documented in CLAUDE.md.
11. **prod.conf overlay check**: production builds have `CONFIG_LOG=n`, no USB device stack,
    but `CONFIG_SERIAL=y` + `CONFIG_UART_1_ASYNC=y` intact.
12. **Build script smoke** (lowest priority): unknown target rejected; env-var/parameter
    precedence in `build.ps1`/`build.sh`.

### Suggested order of attack
1. Test scaffolding + Twister config (item 10 first — it's pure config, no refactor needed).
2. The state-machine tests (item 3) — highest bug density so far.
3. Hook + identity unit tests (items 1–2) alongside the enabling refactor.
4. Flow-control/loopback (items 4–7), then bsim/soak (8–9) as hardware time allows.