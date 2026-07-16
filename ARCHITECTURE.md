# nrfProxy — Architecture Review

Review date: 2026-07-16. Scope: whole repository — `src/main.c`, `prj.conf`, `prod.conf`,
`boards/*`, `CMakeLists.txt`, `build.ps1`/`build.sh`, docs. This document is the
*descriptive* architecture reference plus an assessment; the actionable follow-ups it
feeds live in `ADD_TESTING_PLAN.md` (testing/CI) and `TODO_ARCHITECTURE.md` (code tasks).
`CLAUDE.md` remains the build-environment/gotcha reference; where they overlap, this file
describes *what the system is*, CLAUDE.md describes *how to work on it*.

## 1. What it is

A Zephyr / nRF Connect SDK (NCS v3.3.1, Zephyr 4.3.99) application for nRF52840 boards
that bridges a serial UART to a phone over BLE, bidirectionally:

```
serial device ──UART1 (115200)──► nrfProxy ──BLE notifications (NUS TX)──► phone app
serial device ◄──UART1 TX──────── nrfProxy ◄──BLE writes (NUS RX)───────── phone app
```

The BLE side is the Nordic UART Service (NUS) from the NCS `nrf` module, as a
peripheral, one connection max. The device locks to the first phone that pairs
(bond-to-first-phone; see §6). The production peer is the `bafflingvision` Android app;
generic clients (nRF Connect / nRF Toolbox) work for debugging.

**Design goals, in priority order** (inferred from the code and history):

1. **Unattended robustness** — the bridge must never need a power cycle: advertising
   self-heals (`adv_retry_work`), reconnects always work (the `recycled`-callback
   design), connection teardown is use-after-free-safe.
2. **Low power** — production target is battery-powered (XIAO BLE): two-phase
   advertising, low-duty LED blink, DC/DC where the hardware allows, `prod.conf` strips
   logging/USB.
3. **Loss-tolerant simplicity** — the serial stream is repetitive/safe to drop, so
   every buffer-full / no-listener path *drops* rather than back-pressures.
4. **Extensibility at one seam** — the `on_uart_rx`/`on_ble_rx` interception hooks are
   the designated place for future inspect/modify/filter logic.

## 2. Repository layout

| Path | Role |
|------|------|
| `src/main.c` | The application's Zephyr/BLE glue: UART async driver, BLE/NUS, bridge orchestration, LEDs, pairing lock, identity application. |
| `src/proxy_core.{c,h}` | The pure logic `main.c` decides with: interception hooks, identity derivation, advertising/forwarding predicates, NUS chunk sizing and send-error policy. No Zephyr dependency, by rule — that is what makes it unit-testable. |
| `prj.conf` | Shared Kconfig: BLE peripheral + NUS, pairing/bond persistence, MTU/DLE throughput, async UART. |
| `prod.conf` | Production fragment (opt-in via `EXTRA_CONF_FILE`): drops `CONFIG_LOG` and the USB CDC-ACM console. Must never strip the pairing-lock configs (`BT_SMP`/`BT_SETTINGS`/`NVS`). |
| `boards/<target>.overlay` | Per-board devicetree: enables UART1 + pins, LED role aliases, bond-reset button, DC/DC. Filename = full normalized board target (incl. variant, e.g. `_uf2`). |
| `boards/<target>.conf` | Per-board Kconfig deltas (debug vs size optimization, USB buffer tuning). |
| `build.ps1` / `build.sh` | Committed source of truth for all six build configurations (see §7). |
| `CMakeLists.txt` | Standard Zephyr app boilerplate; compiles only `src/main.c`. |
| `CLAUDE.md`, `README.md`, `PAIRING_PLAN.md`, `TODO.md`, `TODO_ARCHITECTURE.md` | Docs: deep build reference, user-facing overview, pairing design, review/task backlogs. |

There is no test suite and no CI (as of this review) — that is what
`ADD_TESTING_PLAN.md` addresses.

## 3. Execution contexts

Everything interesting about this codebase is concurrency. Six contexts touch shared
state:

| Context | Priority class | Runs |
|---------|----------------|------|
| UART1 ISR (`uart_cb`) | interrupt | RX chunk intake → `on_uart_rx` hook → RX ring put; TX-done chaining (`uart_tx_kick`); RX buffer slab management. |
| BT RX thread (NCS host) | cooperative (negative) | `nus_received` → `on_ble_rx` hook → TX ring put → `uart_tx_kick`. |
| BT host threads | cooperative | Connection callbacks: `on_connected`, `on_disconnected`, `on_recycled`, `on_security_changed`, pairing callbacks. |
| System workqueue | preemptible | Delayed works: `adv_slow_work`, `adv_retry_work`, `adv_blink_work`, `security_timeout_work`. |
| `ble_write_thread` | preemptible, prio 7 | Drains RX ring → `bt_nus_send` chunks sized to ATT MTU − 3. Deliberately below the BT stack threads so pushing data can't starve the stack delivering it. |
| `main()` | — | One-shot init, then returns (the app lives on in the above). |

### Synchronization model (the rules, as implemented)

- **`conn_mutex`** guards the trio `current_conn`, `link_secure`, `adv_active`. Anyone
  using the connection outside the callback that owns it takes its **own
  `bt_conn_ref()` under the mutex** before use and unrefs after (this discipline fixed
  a real connect-then-immediately-disconnect crash).
- **Ring buffers are strictly SPSC.** `uart_rx_ringbuf`: producer = UART ISR, consumer
  = `ble_write_thread`. `uart_tx_ringbuf`: producer = BT RX thread, consumer =
  ISR/thread via `uart_tx_kick`. Consequence: `ring_buf_reset()` is **forbidden**
  (rewrites both indices; corrupts against a concurrent producer — was a real latent
  bug). Disconnect flushing is done from the *consumer* side
  (`uart_rx_ringbuf_drain()`, claim/finish only).
- **`uart_tx_in_progress`** is guarded by `irq_lock()` because kicks come from both
  thread and ISR context; only one `uart_tx()` transfer may be in flight.
- **`locked_mode`** is deliberately *outside* the mutex: single-writer bool, written at
  boot (pre-connection) or from `pairing_complete` (while connected, advertiser
  stopped). (A clarifying comment is pending — `TODO_ARCHITECTURE.md` Task 7.2.)
- **`rx_data_ready`** is a binary semaphore meaning "there may be data"; the consumer
  drains completely per wakeup. It is also the wake mechanism for "state changed,
  re-evaluate" (disconnect flush, security-gate opening).

## 4. Data paths

### Serial → phone

```
UART1 RX (async, double-buffered from uart_rx_slab: 4 × 64 B)
  └─ ISR: UART_RX_RDY ─ on_uart_rx() hook ─► uart_rx_ringbuf (2048 B) ─ k_sem_give
       └─ ble_write_thread: claim ≤ (ATT MTU − 3) ─► bt_nus_send() ─► phone
```

- Sizing rationale (from the code comments): 4 slab buffers because the driver holds
  two (double-buffering) and release can lag the replacement request; 2048 B ring ≈
  180 ms of sustained 115200 — rides out BLE buffer shortages without letting stale
  data build a delay.
- Flow control: `bt_nus_send` returning `-ENOMEM`/`-EAGAIN` keeps the claimed data and
  retries after 10 ms; any other error (not subscribed, disconnected) drops the chunk.
  No connection or unencrypted link → the ring is drained and discarded.
- MTU is re-read every chunk (`bt_gatt_get_mtu`), fallback 20 B; the peripheral never
  initiates MTU exchange — a central stuck at 23 B may be outrun by sustained 115200
  (known, accepted: `TODO.md` I1).
- `UART_RX_BUF_REQUEST` with an empty slab deliberately answers with nothing; the
  driver then raises `UART_RX_DISABLED`, whose handler is *the* recovery point and
  restarts reception. ⚠ If that restart itself fails, RX is dead until reboot —
  known defect, `TODO_ARCHITECTURE.md` Task 3.

### Phone → serial

```
NUS RX write (BT RX thread)
  └─ link_secure gate ─ on_ble_rx() hook ─► uart_tx_ringbuf (2048 B) ─ uart_tx_kick()
       └─ stage ≤64 B in uart_tx_buf ─► uart_tx() … UART_TX_DONE ISR ─ uart_tx_kick() ─ next chunk
```

- `uart_tx()` allows one transfer in flight; chaining happens from the `UART_TX_DONE`
  event. A failing `uart_tx()` start drops the staged chunk (deliberate; the stream is
  loss-tolerant) and clears the flag so the next kick recovers.
- `UART_TX_ABORTED` currently treats the aborted remainder as sent (silent loss —
  `TODO.md` L1 / `TODO_ARCHITECTURE.md` Task 4).

### Interception hooks

`on_uart_rx` / `on_ble_rx` are copy-through stubs: `in` → `out` (separate scratch,
`PROC_BUF_SIZE` = 512), return the output length, 0 = drop, may grow. They see
**transport chunks** (whatever DMA/one GATT write delivered), not framed messages.
⚠ `on_uart_rx` runs in **ISR context** — acceptable for a memcpy stub, wrong for real
filter logic; moving it into `ble_write_thread` is planned
(`TODO_ARCHITECTURE.md` Task 6a) and is a prerequisite for meaningful hook development.

## 5. BLE lifecycle state machine

States (conceptually): `ADVERTISING_FAST` → (30 s, `adv_slow_work`) →
`ADVERTISING_SLOW`; either → `CONNECTED` → (disconnect → object recycled) →
`ADVERTISING_FAST`. Errors route through `adv_retry_work` (1 s) back into
`advertising_start()`.

The three hard-won invariants (each fixed a real field bug — regression-protect these
first, see `ADD_TESTING_PLAN.md`):

1. **Advertising restarts from the `recycled` callback, never from `disconnected`.**
   Inside `on_disconnected` the conn object is still allocated; with
   `CONFIG_BT_MAX_CONN=1`, `bt_le_adv_start()` there fails `-ENOMEM` and the device is
   unreachable until reboot. `recycled` fires once the object is back in the pool and
   covers clean disconnects *and* failed connection attempts.
2. **`recycled` also fires on `bt_le_adv_stop()`** (legacy connectable advertising
   pre-allocates a conn object), so the fast→slow switch would trigger a competing
   second `advertising_start()` (`-EALREADY`). The `adv_active` flag (under
   `conn_mutex`) makes `advertising_start()` a no-op when an advertiser is live or a
   connection exists; `adv_slow_handler` keeps `adv_active=true` across its stop→start
   gap.
3. **The connect-vs-slow-switch race is benign and must stay silent.** If a central
   connects in `adv_slow_handler`'s stop→start gap, the slow start fails `-ENOMEM`
   before `on_connected` has run; the handler re-checks `current_conn` and only lights
   the error LED via the retry path when advertising *genuinely* can't start.

`on_connected` clears `adv_active` in both success and error branches (the attempt
consumed the advertiser either way) and cancels `adv_slow_work`.

## 6. Security architecture (pairing lock)

Full design in `PAIRING_PLAN.md`; summary of the mechanism as built:

- **Two modes decided from stored bonds** (`refresh_locked_mode()`, at boot after
  `settings_load()` and after each pairing). No bond → *pairing mode*, open
  advertising. Bond → *locked mode*: bonded peer's identity address on the **filter
  accept list**, advertising uses `BT_LE_ADV_OPT_FILTER_CONN` params (fast *and* slow
  variants), so strangers are rejected at the link layer. Scan responses stay open
  (deliberate — non-owners can still *see* the device).
- **Just Works, central-driven.** The firmware sends **no** SMP Security Request
  (a peripheral-initiated request made Android show two pairing dialogs — root-caused
  on hardware); the app calls `createBond()` after service discovery.
  `CONFIG_BT_NUS_AUTHEN` is deliberately off (it demands MITM that Just Works can't
  satisfy); instead:
- **App-layer encryption gate**: `link_secure` set by `on_security_changed` at level
  ≥ 2. `nus_received` drops writes and `ble_write_thread` drains-and-discards until
  the link encrypts. Defense-in-depth on top of the accept list.
- **Security watchdog** (`security_timeout_work`): armed on connect, cancelled on
  encryption; disconnects a link that never encrypts, so nothing can squat on the
  single connection slot. Window is per-mode: 10 s locked / 60 s pairing (the pairing
  window must cover the human accepting Android's dialog — disconnecting mid-SMP
  surfaces as "incorrect PIN"). ⚠ Known defect: the 10 s locked window breaks the
  documented "phone forgot the bond, re-pair without factory reset" recovery path —
  fix is `TODO_ARCHITECTURE.md` Task 2 (likely collapses both windows to 60 s).
- **`CONFIG_BT_MAX_PAIRED=1`** — the stack rejects a second bond.
- **Bond reset**: per-board `bond-reset` button held 3 s through boot →
  `bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)` after `settings_load()` →
  `refresh_locked_mode()` → pairing mode. Polling loop pre-`bt_enable`; no ISR
  machinery, by design.
- **Persistence**: `CONFIG_BT_SETTINGS` + settings/NVS in the DT `storage_partition`
  (all four boards define one). `prod.conf` must not strip these.

**Identity** (`identity_init()`): the SoC hardware ID
(`hwinfo_get_device_id`) deterministically derives (a) a fixed static-random BT
address (low 6 bytes + `BT_ADDR_SET_STATIC`, created *before* `bt_enable()`), (b) a
`nrfProxy-XXXX` name suffix (applied after `bt_enable()` via `bt_set_name`;
`ad[]` is non-const because `ad[1].data_len` is patched at runtime), and (c) a 4-byte
per-device ID in the manufacturer AD field (`FF FF 'N' 'P' id×4`; company ID is the
SIG test value — must be replaced before shipping). Stable across reboots without
flash because it's recomputed each boot. Fallback to compile-time defaults if the
hardware ID is unreadable.

## 7. Build & configuration architecture

- **Six configurations**, all defined in `build.ps1`/`build.sh` (the committed source
  of truth; `build*/` is gitignored):

  | Target | Board | Extra config | Flash offset (link) |
  |--------|-------|--------------|---------------------|
  | `dk` | `nrf52840dk/nrf52840` | — | `0x0` (no bootloader) |
  | `xiao` | `xiao_ble/nrf52840` | — | `0x27000` (DT `code_partition`) |
  | `xiao_prod` | `xiao_ble/nrf52840` | `EXTRA_CONF_FILE=prod.conf` | `0x27000` |
  | `promicro` | `promicro_nrf52840/nrf52840/uf2` | — | `0x26000` (DT `code_partition`) |
  | `promicro_prod` | `promicro_nrf52840/nrf52840/uf2` | `EXTRA_CONF_FILE=prod.conf` | `0x26000` |
  | `dongle` | `nrf52840dongle/nrf52840` | — | `0x1000` (`FLASH_LOAD_OFFSET` Kconfig, **not** DT) |

- **Every build passes `-DSB_CONFIG_PARTITION_MANAGER=n`** (a *sysbuild* Kconfig, after
  `--`): the deprecated NCS Partition Manager is off, flash layout comes from
  devicetree partitions. sysbuild itself is kept (hence artifacts under
  `build*/nrfProxy/zephyr/`, and no `merged.hex`). History: PM enabled silently linked
  the Pro Micro at `0x0` → boot loop; this flag is the single highest-value thing CI
  must regression-protect.
- **Config layering**: `prj.conf` (shared) ← `boards/<target>.conf` (auto-merged) ←
  `prod.conf` (opt-in via `EXTRA_CONF_FILE`). Overlay/conf filenames must match the
  full normalized board target including variant (`_uf2`) — a wrong name is *silently
  ignored*.
- **Known config traps** (all documented in CLAUDE.md, all mechanizable as CI
  assertions): wrong-SDK builds (`BT_NUS` undefined against plain Zephyr);
  `CONFIG_UART_1_INTERRUPT_DRIVEN=n` needed or `uart_callback_set()` → `-ENOSYS` on
  USB-console boards; `CONFIG_UDC_BUF_POOL_SIZE` bump for Windows USB enumeration;
  prod builds must keep `SERIAL`/`UART_1_ASYNC` and the pairing-lock configs.

## 8. Per-board differences

| Concern | DK | XIAO BLE | Pro Micro (nice!nano) | Dongle |
|---------|----|----------|----------------------|--------|
| Role | bench/debug | **production** | secondary | bench/debug |
| Console | J-Link VCOM (uart0) | USB CDC-ACM (flaky terminal on boot re-enumeration) | USB CDC-ACM | USB CDC-ACM |
| Flashing | J-Link / `west flash` | UF2 drag-drop | UF2 drag-drop | USB DFU (nrfutil/Programmer) |
| LEDs | led1/2/3 = conn/adv/err | green/blue/red | single LED = conn+adv, **no error LED** (deliberate) | RGB (PWM disabled → GPIO) |
| Bond-reset | SW1 | D2/P0.28 | P0.17 | SW1 (top) |
| DC/DC | board default | enabled in overlay (has inductor) | enabled (genuine nice!nano; disable for stripped clones) | board default |
| LF clock | crystal | crystal | crystal default (RC override needed for crystal-less clones) | crystal |

`main.c` is board-agnostic: board variation is expressed entirely through devicetree
aliases (`led-*`, `bond-reset`) with `GPIO_DT_SPEC_GET_OR` null fallbacks, plus the
per-board `.conf` fragments.

## 9. Assessment

### Strengths

- **The concurrency discipline is explicit and consistently applied** — mutex-guarded
  conn state with ref-counting, SPSC rings with documented reset prohibition,
  irq-locked TX staging. Every rule exists because a real bug was fixed, and the
  comments say so.
- **Failure paths favor availability**: advertising self-heals; RX teardown is
  consumer-side; drops are chosen over deadlock/backpressure everywhere, matching the
  loss-tolerant stream.
- **Board portability is clean**: one C file, all variation in DT aliases + Kconfig
  fragments; adding a board is overlay+conf only.
- **The build system is reproducible-by-script** and its trap-prone flags are
  centralized in the wrappers.
- Documentation density is unusually high; the design rationale (why `recycled`, why
  no Security Request, why PM is off) is written down next to the code.

### Weaknesses / risks

1. **Zero automated tests, zero CI.** The project's bug history is exactly the kind
   that automation catches cheaply: config regressions (PM/0x0 link, `-ENOSYS` async
   gating, prod.conf stripping) and event-ordering state-machine bugs
   (recycled/`-EALREADY`). Every invariant in §5 and §7 is currently protected only
   by comments. → `ADD_TESTING_PLAN.md`.
2. **Monolithic `main.c`** (~1080 lines, all logic static): fine at this size for
   reading, but it makes the pure logic (hooks, identity derivation, state-machine
   predicates, chunking policy) untestable without extraction, and couples the UART
   data path to BT headers. → planned `proxy_core`/`uart_bridge` extraction
   (`ADD_TESTING_PLAN.md` Phase 2/4, `TODO_ARCHITECTURE.md` Task 6).
3. **Hook execution context**: the serial-side extension point runs in ISR context —
   a trap for the very extension work the hooks exist for. (Task 6a.)
4. **Known open defects**: permanent UART-RX death if the `UART_RX_DISABLED` recovery
   fails (Task 3); silent data-loss points with no counters (Task 4); locked-mode
   watchdog vs bond-loss recovery (Task 2).
5. **Single-connection, single-bond design limits** are deliberate but worth naming:
   no coexistence of app + debug client; a second phone requires physical reset.
6. **Hardware-only verification loop**: several behaviors (pairing dialogs, RPA vs
   accept list, address stability across reboots) currently need a human with a
   phone. BabbleSim can simulate a meaningful subset (plan Phase 5); the rest stays a
   documented manual checklist.
7. **Pre-ship items**: SIG test company ID `0xFFFF` in advertising data; the
   `-XXXX` name suffix can collide (16 bits) — benign, documented.

### Testability analysis (what drives the testing plan)

| Logic | Today | Testable how |
|-------|-------|--------------|
| Build/config invariants (offsets, PM off, async UART, prod keeps lock) | manual `.config` greps | Twister build-only matrix + assertion script — **no refactor needed, do first** |
| Hooks, identity derivation, chunk sizing, send-error policy, watchdog window, adv predicates | static in `main.c` | extract to `proxy_core.c` → ztest on `native_sim` |
| UART data path (ISR events, TX chaining, ring flow) | inline in `main.c` with BT includes | extract `uart_bridge.c` (no BT deps) → ztest + `uart_emul` on `native_sim` |
| Adv/conn event sequences (recycled, -EALREADY, races) | comments only | state-decision functions in `proxy_core` (unit) + BabbleSim end-to-end (integration) |
| Pairing lock end-to-end (accept list, encryption gate, watchdog disconnect) | phone in hand | BabbleSim central (stretch) + manual checklist |
| Throughput/soak | phone in hand | BabbleSim soak (stretch); hardware stays authoritative |
