# MITM_PLAN.md — Workstream A: controller ⇄ display datapath

Part of [`FEATURE_PLAN.md`](FEATURE_PLAN.md). Turns the firmware from a one-UART tap into a
true intermediary between the motor controller and the display, and gives it message
boundaries to work with.

**Blocks:** workstream C entirely, and the parts of D that report protocol health.
**Independent of:** workstream B (locking).

---

## The problem in one paragraph

`src/uart_bridge.c` today owns **one** UART: file-scope `uart_rx_ringbuf` / `uart_tx_ringbuf`,
one RX slab, one in-progress flag, one `rx_data_ready` semaphore. Its header documents an SPSC
contract — the RX ring has exactly one producer (the UART ISR) and exactly one consumer
(`ble_write_thread`), and that is what makes the `irq_lock`-only synchronisation correct. A
controller↔display MITM needs two ports and, worse, wants the controller's bytes to reach *two*
destinations (the display, and the phone). Both of those break assumptions the current code is
explicitly built on, so the refactor is staged: devicetree first (A2), then the data structure
(A3), then the new topology (A4).

### The fan-out constraint (decide before writing A3)

Bytes from the controller must go to the display **and** to BLE. Two options:

1. **One consumer thread per link that fans out** — the link's pump thread claims from the RX
   ring once, runs the hook, then pushes the result into the peer link's TX ring *and* to NUS.
   SPSC is preserved exactly as documented; `uart_bridge.h`'s contract survives untouched.
2. **Two rings written by the ISR** — the ISR pushes each byte twice, into a to-display ring
   and a to-BLE ring. Doubles ISR work and duplicates the overflow accounting.

**Recommendation: option 1.** It keeps the contract that the header spends a screen explaining,
it keeps the ISR as dumb as it is now (which was a deliberate fix — see the `on_uart_rx` move
out of ISR context), and the fan-out happens in thread context where the interception logic
already lives.

**The cost to measure:** option 1 adds a thread hop between controller and display. At 115200
that is microseconds of transfer time but a scheduling latency of up to a tick. If the protocol
is request/response with a tight turnaround (many display protocols poll and expect an answer
within a few ms), that budget must be known — which is exactly what A1 measures. **A1's timing
numbers are a gate on A4's design**, not just documentation.

---

## A1 — Capture and document the wire protocol *(no firmware change needed)*

**Why first:** it costs almost nothing, it can be done with the firmware exactly as it stands
today, and everything with a message boundary in it (A5, all of C, parts of D) is guesswork
until it is done.

**How:** the existing single-UART build is already a tap. Wire the controller's TX to the
board's UART1 RX (common ground, 115200 or whatever the bike uses), connect the phone, and
record the NUS stream. Repeat with the *display's* TX to capture the other direction. Both
directions matter: which side initiates tells you whether the link is polled or free-running,
and that decides A4's latency budget.

**Files**
- `PROTOCOL.md` (new) — the findings
- `tests/data/*.bin` or `.hex` (new) — **raw captures kept as test fixtures**; A5's parser
  tests replay these rather than synthetic bytes, so the parser is proven against the real bike
- optionally a `scripts/decode_capture.py` scratch decoder to iterate on hypotheses off-device

**What `PROTOCOL.md` must answer** (these are A5's and C's inputs):
- baud, framing (8N1?), idle voltage/logic level, and whether the two directions share a wire;
- frame delimiters: start byte(s), length field position/width, or is it purely idle-gap
  delimited?
- checksum/CRC algorithm and coverage (which bytes are included);
- message catalogue: which types exist, their cadence, and what each field appears to mean;
- **timing:** inter-frame gap, and if it is request/response, the maximum turnaround the
  display tolerates. Record actual microsecond numbers, not impressions;
- what happens when a frame is malformed or missing — does the display show a fault, retry,
  or ignore it? (This is the safety margin the whole of workstream C operates inside.)

**Tests:** none in the CI sense — the deliverable is the document plus the fixture corpus.
The corpus *is* the test asset; capture generously (idle, riding, error conditions, display
menus, walk mode) so A5 has real garbage to resync from.

**Acceptance:** enough of `PROTOCOL.md` filled in to write a parser against, and at least one
capture per direction committed under `tests/data/`.

**Commit:** `docs: document the controller/display wire protocol with captures`

---

## A2 — Second UART in devicetree; console off UART everywhere

**Why:** the nRF52840 has exactly two UARTE instances. Once both carry bike traffic, no board
can put its console on a UART.

**Files**
- `boards/nrf52840dk_nrf52840.overlay` — enable `uart0` as a data port with chosen pins;
  **move the console to RTT** (`zephyr,console` / `zephyr,shell-uart` chosen nodes)
- `boards/nrf52840dk_nrf52840.conf` — `CONFIG_USE_SEGGER_RTT=y`, `CONFIG_LOG_BACKEND_RTT=y`,
  `CONFIG_RTT_CONSOLE=y`, `CONFIG_UART_CONSOLE=n`
- `boards/xiao_ble_nrf52840.overlay`, `boards/promicro_nrf52840_nrf52840_uf2.overlay`,
  `boards/nrf52840dongle_nrf52840.overlay` — enable the second port with per-board pins
  (their consoles are already USB CDC-ACM, so nothing to move)
- `prj.conf` — `CONFIG_UART_0_INTERRUPT_DRIVEN=n` alongside the existing `UART_1` line. **This
  is the `-ENOSYS` trap from `CLAUDE.md`**: the per-instance async API is silently dropped the
  moment anything enables `CONFIG_UART_INTERRUPT_DRIVEN`, and the USB CDC console does exactly
  that. Getting this wrong costs a runtime `-88` on the boards that matter and nothing on the DK.
- `scripts/check_configs.py` + `scripts/test_check_configs.py` — extend the existing `A8` check
  to cover **both** instances: `CONFIG_UART_0_ASYNC=y` and `CONFIG_UART_1_ASYNC=y`,
  `*_INTERRUPT_DRIVEN=n` for both.
- `README.md` / `CLAUDE.md` — the DK's console is now RTT (`JLinkRTTViewer` or
  `west attach`-style flow); document it where the UART pinout table lives.

**Tests**
- `python scripts/test_check_configs.py` gains fixtures for the two-instance assertion
  (positive and negative), and `check_configs.py` then fails any target that regresses it.
- All six build targets must build and keep their flash offsets (`0x0` / `0x27000` / `0x26000`
  / `0x1000`) and emit no `partitions.yml` — the existing checks already cover that; this task
  just must not break them.

**Acceptance:** six green builds, `check_configs.py` passes, and a boot log is visible over RTT
on the DK.

**Commit:** `feat(uart): enable a second async UART on every board; DK console moves to RTT`

---

## A3 — `uart_bridge` singleton → instantiable `struct uart_link` *(behaviour-preserving)*

**Why:** every file-scope object in `uart_bridge.c` is currently implicitly "the" port. Nothing
else can be added until they belong to an instance. **This commit changes no behaviour** — one
link is instantiated and wired exactly as before — which is what makes it safely reviewable and
lets the existing integration suite act as the regression net.

**Files**
- `src/uart_bridge.h` / `src/uart_bridge.c` — introduce `struct uart_link` holding the two ring
  buffers, the RX slab (or a per-link slab, see below), the TX-in-progress flag, the staging
  buffer, the drop counters, and the data-ready semaphore. All existing functions take a
  `struct uart_link *`.
- `src/main.c` — declare the single link, pass it through.
- `tests/integration/uart_bridge/src/main.c` — adapted to the new API.

**Care points**
- **The SPSC contract in `uart_bridge.h` must be rewritten, not just relocated.** It currently
  says "one producer (ISR), one consumer (`ble_write_thread`)". Per link that stays true; state
  it per link, and state explicitly that a link's RX ring still has exactly one consumer thread
  even after A4 introduces fan-out.
- **`k_mem_slab` cannot live inside a runtime struct** — `K_MEM_SLAB_DEFINE` is a static
  object. Either define one slab per link statically and hand the link a pointer, or use a
  single shared slab sized for both links. **Recommendation: one slab per link**, so a stalled
  display port cannot starve the controller port of RX buffers.
- Same for `K_SEM_DEFINE` — define per link, pass the pointer in, exactly as
  `uart_bridge_init(uart_dev, &rx_data_ready)` already does today.
- Keep the `irq_lock()` window in `uart_tx_kick()` as small as it is now (`TODO.md` I2).

**Tests**
- The **existing** `tests/integration/uart_bridge` suite must pass with only mechanical changes
  (an instance argument). Any behavioural diff here is a bug in the refactor.
- **Add a second instance to the suite**: two `uart_emul` devices, two links, traffic driven
  through both simultaneously, asserting complete independence — bytes never cross, one link's
  ring overflow does not perturb the other, and the per-link drop counters attribute correctly.
  This is the test that proves no shared static survived.

**Acceptance:** integration suite green with one and two instances; all six targets build; no
behavioural change on hardware (spot-check with the existing single-port setup).

**Commit:** `refactor(uart): make uart_bridge instantiable as struct uart_link`

---

## A4 — Passthrough: controller ⇄ display, with BLE as an observer

**Why:** this is the commit where the box actually sits in the wire.

**Files**
- `src/main.c` — two links, a pump thread per direction, fan-out to the peer link's TX plus NUS
- `src/proxy_core.c/.h` — the hooks gain a direction/link identity so C can tell
  controller-originated from display-originated data. Keep the existing signatures working, or
  extend them in one deliberate step; either way `main.c` BUILD_ASSERTs the coupling.
- `tests/integration/passthrough/` (new suite)

**Design requirements**
- **Passthrough must not depend on BLE.** If no phone is connected, if the link is unencrypted,
  or if the BLE stack is wedged, the controller and display must still talk. Today's "no
  connection → discard buffered data" behaviour is right for the *BLE* copy and wrong for the
  *display* copy; make the two paths independent, with the peer-UART write being the one that
  never gets skipped.
- **Back-pressure asymmetry.** If the display's TX ring fills, dropping bytes corrupts frames.
  Prefer dropping the *BLE* copy under pressure and keeping the wire copy intact — the wire is
  the bike, BLE is a spectator. Record both drop classes separately (A6).
- Honour the latency budget A1 measured. If the protocol turns out to be tight request/response,
  reconsider the thread hop (fan-out constraint above) before shipping this.

**Tests** (`tests/integration/passthrough` on `native_sim`, two `uart_emul` devices)
- bytes written to the controller-side emulated UART appear **unmodified** on the display-side
  one, and vice versa, including across ring wrap-around;
- a burst larger than the ring exercises the drop path without corrupting frame-sized runs that
  do fit;
- with no BLE connection, passthrough still works (the regression the design requirement above
  exists to prevent);
- measured end-to-end latency inside the emulator stays under the budget from A1 (assert it as
  a number, so a future refactor that adds a hop fails the test rather than the bike).

**Acceptance:** the display works on the bike with the proxy inline and no phone connected —
the "did we break the bike" gate for the whole programme.

**Commit:** `feat(uart): pass controller and display traffic through the bridge`

---

## A5 — `frame.c`: message framing and reassembly

**Why:** hooks currently see transport chunks (`proxy_core.h` is explicit and honest about it).
Nothing in workstream C can safely modify a message it cannot delimit.

**Files:** `src/frame.h`, `src/frame.c` (new, Zephyr-free), `tests/host/test_frame.c`,
`tests/unit/frame/` (the same logic under ztest on `native_sim`), `run.ps1`, `CMakeLists.txt`.

**Design:** a resumable parser — feed it arbitrary chunks, it emits complete messages and keeps
partial state. Whatever `PROTOCOL.md` says the delimiting rule is (start byte + length +
checksum, or idle-gap), the parser must handle:
- a message split across any number of chunk boundaries (including one byte at a time);
- multiple messages in one chunk;
- **resynchronisation from garbage** — a corrupt byte must not desynchronise the stream
  permanently; this is the property that decides whether the display glitches for one frame or
  forever;
- a maximum message length guard so a lost length byte cannot make it buffer without bound;
- checksum validation, with invalid frames counted and dropped rather than passed on;
- an idle/timeout flush if the protocol is gap-delimited (milliseconds cross the boundary, not
  `k_timeout_t` — the `proxy_core` rule).

**Tests** — this is the module with the highest bug density in the programme, so:
- **replay the real captures from A1** (`tests/data/`) and assert the expected message count
  and types come out — the single most valuable test here;
- feed the same capture in 1-byte chunks, in prime-sized chunks, and in one big chunk: identical
  output every time (chunk-size invariance is the property that catches most parser bugs);
- inject single-bit corruption at every offset of a known-good capture and assert the parser
  resyncs within one message and counts exactly one checksum failure;
- truncated trailing message leaves clean partial state, completed by the next chunk;
- length-field abuse (claimed length beyond the max) hits the guard, does not over-read;
- zero-length input and a chunk of pure garbage.

**Acceptance:** host + ztest suites green; parsed message counts match hand-decoded captures.

**Commit:** `feat(frame): add a resumable, resyncing message parser with capture-replay tests`

---

## A6 — Per-link drop accounting

**Why:** `src/drop_stats.c` already exists and is host-tested; with two links and a fan-out,
"drops" is now several distinct numbers and lumping them together loses the diagnosis.

**Files:** `src/drop_stats.c/.h`, `src/uart_bridge.c`, `tests/host/test_drop_stats.c`.

**Counters:** per link — RX ring overflow, TX start failure, TX aborted remainder; plus, new
with the fan-out — BLE-copy drops (expected under pressure, benign) kept **separate** from
wire-copy drops (never expected, always a bug or a hardware problem). Frame-level counters
(checksum failures, resyncs) land here too once A5 exists.

**Tests:** extend the existing host suite — per-link attribution, saturation/wrap behaviour of
the counters, and that the periodic reporting still only logs when something changed.

**Commit:** `feat(stats): account drops per link and per fan-out destination`

---

## A7 — *(optional, hardware)* Fail-safe passthrough bypass

Gate on Q6 in `FEATURE_PLAN.md` §4. Today, if the firmware hangs, the display loses the
controller — the bike becomes undiagnosable to its rider mid-ride.

Options, roughly in cost order:
- **Watchdog** (`CONFIG_WDT`) that resets the SoC on a stalled pump thread. Cheap, software
  only, but the link is still dead for the reset duration.
- **Normally-closed analog switch / relay** wiring controller TX straight to display RX
  whenever the MCU is unpowered or a heartbeat GPIO stops toggling. True bypass, costs a part.
- **Accept the risk** and document it — defensible if A1 shows the display degrades gracefully
  when the controller goes quiet.

No code is scheduled here; it is listed so the decision is explicit rather than implicit.

---

## Open items for this workstream

- Q1 (which protocol) and Q2 (two wires or one shared half-duplex line) — A1 answers both. If
  the link turns out to be **half-duplex on a single wire**, A2/A3/A4 change shape considerably
  (one UART with direction control, or a single-wire tap plus injection), so treat A1's answer
  as a gate before starting A2.
- The A1 latency numbers gate A4's fan-out design (see the fan-out constraint above).
