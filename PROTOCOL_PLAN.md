# PROTOCOL_PLAN.md — Workstream A: controller protocol

Part of [`FEATURE_PLAN.md`](FEATURE_PLAN.md). Gives the firmware an understanding of the motor
controller's wire format, and message boundaries to work with instead of transport chunks.

**Blocks:** workstream C entirely, and the parts of D that report protocol health.
**Independent of:** workstream B (locking).

---

## The topology (settled 2026-07-28)

```
Motor → Motor Controller → nRF52840 UART1 → nRF52840 BLE (NUS) → Android BLE → Android app
                                                                                (the display)
```

**The Android app *is* the display.** There is no physical display unit in the serial chain, so
the firmware is not an intermediary between two serial peers — it is a **serial ⇄ Bluetooth LE
bridge**, which is exactly what it already is. The controller↔nRF link is two wires, full
duplex: `uart1` TX and RX. That is one UART, already wired, already working.

**Consequence: no datapath refactor is needed.** An earlier draft of this plan assumed a
physical display downstream and specified a second UART, a `uart_bridge` singleton refactor, a
passthrough path, and a console move to RTT on the DK. None of that is required — the topology
above needs one UART and the existing bridge. This workstream is now only the tasks that were
never about topology: know the protocol (A1), parse it (A2), and account for parse failures (A3).

### The two directions, and their differing risk

| Direction | Path | What it carries | Risk if we get it wrong |
|---|---|---|---|
| **Uplink** | controller → `uart1` RX → ring → `ble_write_thread` → NUS notify → app | Telemetry the app renders as a display | Low — a bad parse shows a wrong number on a phone |
| **Downlink** | app → NUS write → `nus_received` → `on_ble_rx` → `uart_bridge_send` → `uart1` TX → controller | Commands: assist level, walk mode, settings | **This is the direction that matters.** The controller acts on these |

Both paths exist and work today. The uplink is where interception adds value (workstream C);
the downlink is where frame correctness is a requirement rather than a nicety — a malformed
frame goes to something that drives a motor.

---

## A1 — Capture and document the controller protocol *(no firmware change needed)*

**Why first:** it costs almost nothing, it can be done with the firmware exactly as it stands,
and everything with a message boundary in it (A2, all of C, parts of D) is guesswork until it
is done.

**How:** the firmware already forwards everything it receives to the phone, which is all a
capture needs. Wire the controller to `uart1` as usual, connect the app (or nRF Connect),
record the NUS stream, and exercise the bike through the states worth capturing.

For the **downlink**, capture what the app sends and what the controller does in response. If a
stock display is available to borrow signal from, recording its commands is much the fastest
way to learn the command set; otherwise this is careful, incremental probing.

**Files**
- `PROTOCOL.md` (new) — the findings
- `tests/data/*.bin` (new) — **raw captures kept as test fixtures**; A2's parser tests replay
  these rather than synthetic bytes, so the parser is proven against the real controller
- optionally `scripts/decode_capture.py` — a scratch decoder to iterate on hypotheses off-device

**What `PROTOCOL.md` must answer** (these are A2's and C's inputs):
- baud, framing (8N1?), and confirmation of the full-duplex two-wire pinout;
- frame delimiters: start byte(s), length field position/width, or purely idle-gap delimited?
- checksum/CRC algorithm and which bytes it covers;
- **uplink message catalogue** — which types exist, their cadence, what each field means
  (speed, battery, current, error codes, assist level…). This is the app's data model;
- **downlink command catalogue** — what the controller accepts, and critically **what it does
  with a malformed or unexpected command** (ignore? fault? undefined?). That is the safety
  margin every command the app sends operates inside;
- timing: inter-frame gap, and whether the controller polls, free-runs, or is request/response.

**Tests:** none in the CI sense — the deliverable is the document plus the fixture corpus. The
corpus *is* the test asset; capture generously (idle, riding, low battery, error conditions,
each assist level, walk mode) so A2 has real garbage to resync from.

**Acceptance:** enough of `PROTOCOL.md` filled in to write a parser against, and captures of
both directions committed under `tests/data/`.

**Commit:** `docs: document the motor controller wire protocol with captures`

---

## A2 — `frame.c`: message framing and reassembly

**Why:** hooks currently see transport chunks — `proxy_core.h` is explicit and honest that
`on_uart_rx` sees "whatever the ring held contiguously, capped at one notification's worth",
which is not the controller's chunking. Nothing in workstream C can act on a message it cannot
delimit, and today the app is reassembling frames itself out of arbitrary BLE chunks.

**Files:** `src/frame.h`, `src/frame.c` (new, Zephyr-free — the `proxy_core.h` binding rule),
`tests/host/test_frame.c`, `tests/unit/frame/` (same logic under ztest on `native_sim`),
`tests/host/run.ps1`, `CMakeLists.txt`.

**Design:** a resumable parser — feed it arbitrary chunks, it emits complete messages and keeps
partial state across calls. Whatever `PROTOCOL.md` says the delimiting rule is, it must handle:
- a message split across any number of chunk boundaries (including one byte at a time);
- multiple messages in one chunk;
- **resynchronisation from garbage** — a corrupt byte must not desynchronise the stream
  permanently;
- a maximum message length guard, so a lost length byte cannot buffer without bound;
- checksum validation, with invalid frames counted and dropped rather than passed on;
- an idle/timeout flush if the protocol is gap-delimited (milliseconds cross the boundary, not
  `k_timeout_t`).

**A design question A1 answers:** whether the firmware should send the app *frames* rather than
raw bytes. If it does, the app stops reassembling and gets one notification per message — a
real simplification on the Android side, at the cost of the firmware being wrong in a way the
app can no longer work around. **Recommendation: keep the raw passthrough as-is and add framing
as an additive capability**, so the app can migrate when it wants and the raw byte stream
remains the ground truth for debugging.

**Tests** — highest bug density in the programme, so:
- **replay the real captures from A1** and assert expected message count and types — the single
  most valuable test here;
- the same capture fed in 1-byte chunks, prime-sized chunks, and one big chunk produces
  **identical** output (chunk-size invariance catches most parser bugs);
- single-bit corruption injected at every offset of a known-good capture: the parser resyncs
  within one message and counts exactly one checksum failure;
- truncated trailing message leaves clean partial state, completed by the next chunk;
- length-field abuse (claimed length beyond max) hits the guard without over-reading;
- zero-length input, and a chunk of pure garbage.

**Commit:** `feat(frame): add a resumable, resyncing message parser with capture-replay tests`

---

## A3 — Frame-level health counters

**Why:** `src/drop_stats.c` already exists and is host-tested for the transport-level silent
loss points (`TODO.md` L1). Framing adds a new class of loss worth separating: bytes that
arrived fine but did not form a valid message.

**Files:** `src/drop_stats.c/.h`, `src/frame.c`, `tests/host/test_drop_stats.c`.

**Counters:** checksum failures, resync events, over-length rejections, and bytes discarded
during resync — kept **separate** from the existing ring-overflow and TX-failure counters,
because they mean completely different things (EMI or a wiring fault versus a throughput
problem). These feed workstream D's snapshot.

**Tests:** extend the existing host suite — per-class attribution, counter saturation/wrap, and
that the periodic reporting still only logs when something changed.

**Commit:** `feat(stats): count frame-level parse failures separately from transport drops`

---

## What this workstream no longer contains

Recorded so the deleted work is not silently re-invented:

- ~~Second UART in devicetree; DK console to RTT~~ — no second serial peer exists.
- ~~`uart_bridge` singleton → `struct uart_link` refactor~~ — one link is all there is. The
  SPSC contract in `uart_bridge.h` stands unchanged and untouched.
- ~~Controller ⇄ display passthrough with BLE fan-out~~ — the app is the display; the existing
  uplink already is the path to it.
- ~~Fail-safe passthrough bypass for a firmware hang~~ — a hang costs the *app's* display, not
  the bike's ability to run. Worth a watchdog eventually; not a safety-critical bypass.

---

## Open items

- **Q1 (which protocol)** is answered by A1 and gates A2 and all of workstream C.
- Whether to send framed messages or raw bytes to the app (see A2) — decide with the app in
  view, once `PROTOCOL.md` exists.
- The downlink command set is the part with real consequences. If A1 cannot establish what the
  controller does with a malformed command, treat every new command the app sends as
  bench-tested-before-ridden.
