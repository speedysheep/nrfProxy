# nrfProxy — Code Review Findings & Test Plan

Review date: 2026-07-02 (post advertising-restart/`recycled` fixes, pre-commit).
Scope: whole project — `src/main.c`, `prj.conf`, `prod.conf`, `boards/*`, build scripts, docs.
No code was changed as part of this review; every item below is a TODO.

Severity scale: **High** = real defect likely to bite in normal use · **Medium** = defect or
gap that bites under specific conditions · **Low** = minor/cosmetic/robustness ·
**Info** = awareness item, no action strictly required.

---

## Issues (ranked by severity)

### H1. ~~`ring_buf_reset()` races the UART ISR producer — potential ring-buffer corruption~~ ✅ RESOLVED (2026-07-02)
- **Resolution:** both `ring_buf_reset()` calls on `uart_rx_ringbuf` removed.
  `on_disconnected` now just wakes the writer thread (`k_sem_give(&rx_data_ready)`), which
  sees `current_conn == NULL` and discards via the new `uart_rx_ringbuf_drain()` — a
  claim/finish loop, i.e. a pure consumer-side operation that is safe against the ISR
  producer. `uart_tx_ringbuf` was checked and has no reset call (its consumer side is
  serialized under `irq_lock` in `uart_tx_kick`), so it needed no change. All six build
  configs compile; runtime disconnect-while-streaming test still to be done on hardware.
- Original finding follows for reference.
- Where: `src/main.c:396` (`on_disconnected`) and `src/main.c:596` (`ble_write_thread`, the
  "nobody listening" discard).
- Zephyr ring buffers are only concurrency-safe as single-producer/single-consumer where the
  producer touches the head and the consumer the tail. `ring_buf_reset()` rewrites **both**
  indices, so it is neither role. The producer here is the UART ISR (`UART_RX_RDY` →
  `ring_buf_put`, `src/main.c:506`), which keeps firing after a disconnect (the serial side
  doesn't know the phone left). A reset from the BT host thread or writer thread concurrent
  with an ISR `put` can corrupt the indices → garbled forwarded data or a permanently
  confused buffer.
- This window is hit on **every disconnect while serial data is streaming**, which for the
  e-bike use case is the normal case.
- Fix direction (later): flush from the consumer side only (loop
  `ring_buf_get_claim`/`ring_buf_get_finish` in `ble_write_thread`), or set a `flush` flag the
  writer thread acts on, or guard the reset with `irq_lock()`. Same treatment for both sites.

### M1. ~~Error states are terminal — no retry/recovery, device stays dead until power cycle~~ ✅ RESOLVED for advertising (2026-07-02)
- **Resolution:** a dedicated `adv_retry_work` (1 s cadence, `ADV_RETRY_DELAY`) is now
  scheduled by every advertising failure path: `advertising_start()` failure,
  `adv_slow_handler()` start failure (retry restores fast advertising, the slow switch then
  follows again), and `adv_slow_handler()` stop failure (still advertising fast, so it
  re-schedules the switch itself instead). The error LED shows during the retry window and
  clears on recovery. Stale retries after recovery/connection are no-ops via the
  `current_conn`/`adv_active` guard in `advertising_start()`, so nothing needs cancelling.
  All six build configs compile.
- **Deliberately NOT covered:** `main()` init failures (`uart_init`, `bt_enable`,
  `bt_nus_init`) remain terminal — those indicate hardware/config faults where a retry loop
  would just mask the problem. A hardware watchdog (`CONFIG_WATCHDOG`) remains the right
  future companion for field robustness (unmoved from this list — see below).
- Original finding follows for reference.
- Where: `advertising_start()` failure path (`src/main.c:308-312`), `adv_slow_handler()`
  failure path (`src/main.c:347-351`), and `main()` init failures.
- Any transient `bt_le_adv_start()` failure (e.g. a momentary buffer/conn-object shortage)
  lights the error LED and stops — nothing ever retries, so a recoverable hiccup bricks the
  bridge until reboot. For an unattended device this should self-heal.
- Fix direction: on adv-start failure, reschedule a retry (e.g. via `adv_slow_work` or a
  dedicated retry work item with backoff) instead of dead-ending. Consider the same for
  `uart_init()` failure. A hardware watchdog (`CONFIG_WATCHDOG`) is the belt-and-braces
  companion for a headless bridge.

### M2. UART RX can stop permanently if buffer recovery fails
- Where: `UART_RX_DISABLED` handler, `src/main.c:525-534`.
- If `k_mem_slab_alloc()` (K_NO_WAIT, in ISR) or `uart_rx_enable()` fails at that moment,
  reception is never re-enabled — the serial→BLE direction silently dies while everything
  else (LEDs, BLE) looks healthy. Slab exhaustion is unlikely (4 buffers, released via
  `UART_RX_BUF_RELEASED`) but not impossible under event-ordering edge cases.
- Fix direction: on failure, schedule a delayed work item that retries `uart_rx_enable()`
  until it succeeds, and log once.

### M3. Unauthenticated, unencrypted NUS — anyone in radio range can drive the serial port
- The NUS RX characteristic accepts writes from any connected central; there is no pairing,
  bonding, or encryption requirement. Whoever connects first owns the bridge — and the
  serial peer here is an e-bike motor controller (assist level, walk mode, etc. flow over
  this link via the phone app).
- Known/accepted for now (project explicitly excludes security hardening while
  open-sourcing), but record the decision and threat model deliberately rather than by
  omission. If it's ever revisited: `CONFIG_BT_SMP` + security level on the NUS
  characteristics, or an application-level allowlist using the peer address.

### M4. CLAUDE.md is stale on the Pro Micro LF-clock config (says the opposite of the code)
- CLAUDE.md ("LF clock forced to the internal RC oscillator…") still documents
  `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` living in `boards/promicro_nrf52840_nrf52840_uf2.conf`,
  but commit `f678d25` restored the crystal default; the `.conf` now deliberately has **no
  override** and documents the inverse recommendation (add RC only for crystal-less clones).
- CLAUDE.md is this repo's deep reference — a future session following it would "restore" a
  setting that was intentionally removed. Update the paragraph (and the Status section if it
  mentions RC).

### L1. Silent data loss points — no counters, no logs
- `uart_rx_ringbuf` overflow: partial `ring_buf_put()` in the ISR (`src/main.c:506`) ignores
  the return value; bytes vanish without trace. (2048 bytes ≈ 180 ms of 115200 stream — a
  slow/backgrounded phone can overflow it while connected.)
- `uart_tx()` start failure drops the staged chunk with no log (`src/main.c:441-443`), and
  `UART_TX_ABORTED` treats the aborted remainder as sent (`src/main.c:489-494`).
- Fix direction: keep drop **counters** (ISR-safe) and log them periodically from thread
  context; log the `uart_tx()` error code once.

### L2. Transient wrong LED / error log if a central connects exactly at the fast→slow switch
- `adv_slow_handler()` (`src/main.c:321-354`) can run in the window where the controller has
  already accepted a connection but `on_connected` hasn't updated state; its
  `bt_le_adv_start(slow)` then fails (no free conn object) → error LED + log, corrected a
  moment later by `on_connected`. Harmless but confusing during debugging. Accept and
  document, or have the failure path re-check `current_conn` before declaring error.

### L3. LED state machine has benign races and a cosmetic blink quirk
- `set_status_leds()` / `current_status` are touched from main, BT host thread, and the
  system workqueue with no synchronization; interleaved calls could briefly light two LEDs.
- `adv_blink_handler`'s `static bool on` (`src/main.c:129`) carries over between advertising
  sessions, so the first blink after re-entering advertising can start with an off-gap.
- Cosmetic only; fold into any future LED rework.

### L4. README nits
- "Per-board Kconfig fragments" table labels the XIAO and Pro Micro rows "(USB DFU)" — both
  flash by UF2 drag-and-drop (only the Dongle is USB DFU). The main board table has it right.
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