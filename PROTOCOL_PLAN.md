# PROTOCOL_PLAN.md — Workstream A: controller protocols

Part of [`FEATURE_PLAN.md`](FEATURE_PLAN.md). Gives the firmware an understanding of the motor
controller's wire format — **for at least four different protocols at different baud rates** —
and message boundaries to work with instead of transport chunks.

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

**Consequence: no datapath refactor is needed.** An earlier draft assumed a physical display
downstream and specified a second UART, a `uart_bridge` singleton refactor, a passthrough path
and a console move to RTT on the DK. None of that is required. This workstream is the tasks
that were never about topology: know the protocols (A1), parse them (A2), account for parse
failures (A3), and pick the right one at runtime (A4).

### The two directions, and their differing risk

| Direction | Path | What it carries | Risk if we get it wrong |
|---|---|---|---|
| **Uplink** | controller → `uart1` RX → ring → `ble_write_thread` → NUS notify → app | Telemetry the app renders as a display | Low — a bad parse shows a wrong number on a phone |
| **Downlink** | app → NUS write → `nus_received` → `on_ble_rx` → `uart_bridge_send` → `uart1` TX → controller | Commands: assist level, walk mode, settings | **This is the direction that matters.** The controller acts on these |

---

## Resource budget (measured/derived 2026-07-28)

The headline: **CPU is not a constraint for packet manipulation, by roughly three orders of
magnitude.** nRF52840 is a Cortex-M4F at 64 MHz.

| Baud | Bytes/sec | CPU cycles available per byte |
|---|---|---|
| 115200 | 11,520 | ~5,500 |
| 9600 | 960 | ~66,000 |
| 1200 | 120 | ~533,000 |

A table-driven CRC-16 is 5–10 cycles/byte; parsing, validating, rewriting and re-checksumming a
16-byte frame is a few hundred cycles. Running all four parsers concurrently during detection
(A4) is affordable.

**Flash:** the image is ~232 KB of 1 MB (CI-verified), leaving ~790 KB. Four protocol
descriptors cost a few KB.

**What actually constrains us:**

- **BLE throughput, not CPU.** `TODO.md` I1: the firmware advertises support for a 247-byte MTU
  but as a peripheral never initiates the exchange. A central that stays at the 23-byte default
  gets 20-byte notifications, and at a 30–50 ms connection interval that caps the uplink well
  below what the UART can deliver. This is the ceiling on how much C can add to the uplink.
- **RAM.** 256 KB total, with the BLE stack taking a large share. Current fixed allocations:
  `uart_rx_ringbuf` 2048 B, `uart_tx_ringbuf` 2048 B, `uart_rx_slab` 4×64 B, two 512 B
  `PROC_BUF_SIZE` scratch buffers, 2048 B main stack, 2048 B `ble_write_thread` stack.
  **Not yet measured** — get the real number with a build plus
  `arm-zephyr-eabi-size build_devkit/nrfProxy/zephyr/zephyr.elf`, and record it in the findings
  log. Per-protocol reassembly state must be **one active parser's worth**, not four
  simultaneously live, or this is where it shows.
- **Wire time dwarfs processing.** At 1200 baud a 16-byte frame takes 133 ms to transmit. No
  amount of parsing competes with that; latency budgets are set by the wire, not the CPU.

---

## A1 — Capture and document the protocols *(no firmware change needed)*

**Why first:** it costs almost nothing, it can be done with the firmware exactly as it stands,
and everything with a message boundary in it (A2, A4, all of C, parts of D) is guesswork until
it is done.

**How:** the firmware already forwards everything it receives to the phone, which is all a
capture needs. Wire the controller to `uart1`, connect the app (or nRF Connect), and exercise
the bike through the states worth capturing.

**⚠ The practical constraint on this task is access, not effort.** Four protocols means four
controllers to capture from. Options, in descending order of confidence: capture from real
hardware you have; borrow a stock display for a system you don't (recording *its* commands is
much the fastest way to learn a command set); or work from published/community reverse
engineering, treating it as a hypothesis to be confirmed against a real capture before any
downlink command is sent. **Do not ship downlink support for a protocol you have never
captured** — the uplink is guessable, the command set is not.

**Files**
- `PROTOCOL.md` (new) — one section per protocol
- `tests/data/<protocol>/*.bin` (new) — **raw captures kept as test fixtures**, per protocol;
  A2's parser tests replay these rather than synthetic bytes
- optionally `scripts/decode_capture.py` — a scratch decoder to iterate on hypotheses off-device

**What `PROTOCOL.md` must answer, per protocol:**
- **baud rate** and framing (8N1?) — these differ per protocol and drive A4;
- frame delimiters: start byte(s), length field position/width, or purely idle-gap delimited?
- checksum/CRC algorithm and which bytes it covers;
- **uplink message catalogue** — types, cadence, field meanings (speed, battery, current, error
  codes, assist level…). This is the app's data model;
- **downlink command catalogue** — what the controller accepts, and critically **what it does
  with a malformed or unexpected command** (ignore? fault? undefined?);
- timing: inter-frame gap, and whether the controller polls, free-runs, or is request/response;
- **anything that distinguishes this protocol from the others on the wire** — the discriminator
  A4 uses to tell them apart.

**Tests:** none in the CI sense — the deliverable is the document plus the fixture corpus. The
corpus *is* the test asset; capture generously (idle, riding, low battery, error conditions,
each assist level, walk mode) so A2 has real garbage to resync from.

**Commit:** `docs: document the <name> controller protocol with captures` *(one per protocol —
each is independently commitable and independently useful)*

---

## A2 — `frame.c`: pluggable multi-protocol framing

**Why:** hooks currently see transport chunks — `proxy_core.h` is explicit that `on_uart_rx`
sees "whatever the ring held contiguously, capped at one notification's worth", which is not
the controller's chunking. And with four protocols, this cannot be one hardcoded parser.

**Files:** `src/frame.h`, `src/frame.c` (new, Zephyr-free — the `proxy_core.h` binding rule),
`src/proto/<name>.c` (one per protocol), `tests/host/test_frame.c`, `tests/unit/frame/`,
`tests/host/run.ps1`, `CMakeLists.txt`.

**Design: one generic state machine, one descriptor per protocol.** The resync, chunk-boundary
and bounds logic is written and tested *once*; each protocol contributes a small table and two
or three functions.

```c
struct proto_desc {
        const char *name;
        uint32_t    baud;              /* differs per protocol — drives A4 */
        size_t      min_len, max_len;

        /* Offset of the next plausible frame start in buf, or -1 if none.
         * This is the resync primitive. */
        int  (*find_sof)(const uint8_t *buf, size_t len);
        /* Total frame length from a partial header; -1 = need more bytes. */
        int  (*frame_len)(const uint8_t *hdr, size_t len);
        bool (*checksum_ok)(const uint8_t *frame, size_t len);
        /* Recompute in place after modification (workstream C). */
        void (*checksum_set)(uint8_t *frame, size_t len);
        uint8_t (*msg_type)(const uint8_t *frame, size_t len);
};
```

The parser is resumable — fed arbitrary chunks, it emits complete messages and keeps partial
state. Requirements, for every descriptor:
- a message split across any number of chunk boundaries (including one byte at a time);
- multiple messages in one chunk;
- **resynchronisation from garbage** — a corrupt byte must not desynchronise permanently;
- a maximum message length guard, so a lost length byte cannot buffer without bound;
- checksum validation, with invalid frames counted and dropped rather than passed on;
- an idle/timeout flush for gap-delimited protocols (milliseconds cross the boundary, not
  `k_timeout_t`).

**Only one parser instance is live at a time** (see the RAM note in the budget above) — the
descriptor is selected by A4, not compiled in exclusively.

**A design question A1 answers:** whether the firmware should send the app *frames* rather than
raw bytes. If it does, the app stops reassembling out of arbitrary BLE chunks and gets one
notification per message — a real simplification on the Android side, and it gets larger with
four protocols to support. **Recommendation: keep raw passthrough as the ground truth and add
framing as an additive capability**, so the app migrates when it wants and debugging still has
the unfiltered stream.

**Tests** — the highest bug density in the programme, and now parameterised over descriptors:
- **replay the real captures from A1** per protocol, asserting expected message count and types;
- **the generic suite runs against every descriptor** — chunk-size invariance (1-byte, prime,
  and whole-buffer feeds produce identical output), resync-from-corruption within one message,
  truncated trailing message, length-field abuse hitting the guard, zero-length and pure garbage;
- **cross-protocol negative test:** protocol A's capture fed to protocol B's descriptor must
  produce few or no "valid" frames. If it produces many, the two are not distinguishable and
  A4's detection cannot work — that is a finding, and it is much cheaper to learn here.

**Commit:** `feat(frame): add a resumable multi-protocol message parser` *(then one commit per
`src/proto/<name>.c` descriptor)*

---

## A3 — Frame-level health counters

**Why:** `src/drop_stats.c` already exists and is host-tested for the transport-level silent
loss points (`TODO.md` L1). Framing adds a class of loss worth separating: bytes that arrived
fine but did not form a valid message.

**Files:** `src/drop_stats.c/.h`, `src/frame.c`, `tests/host/test_drop_stats.c`.

**Counters:** checksum failures, resync events, over-length rejections, bytes discarded during
resync — **separate** from ring-overflow and TX-failure counters, because they mean completely
different things (EMI or a wrong protocol selected, versus a throughput problem). A sudden rise
in checksum failures is also the signal that A4 picked the wrong protocol, so these feed both
workstream D's snapshot and A4's confidence check.

**Tests:** extend the existing host suite — per-class attribution, counter saturation/wrap, and
that periodic reporting still only logs when something changed.

**Commit:** `feat(stats): count frame-level parse failures separately from transport drops`

---

## A4 — Protocol and baud selection

**Why:** four protocols at different baud rates means the firmware must know which one it is
talking to. **UART cannot negotiate this** — it is asynchronous with no handshake, and the
nRF52840's UARTE has no hardware auto-baud detect. So it is either configured or detected in
software.

**Design, in priority order:**

1. **App-configured, and authoritative.** The rider knows which bike they are on. Add
   `SET_PROTOCOL` to the control service's command set (B3), persist the choice in settings
   (alongside B6's preferences), and apply it at boot. This is the primary mechanism: an
   explicit setting is wrong only if the user picks wrong, and visibly so — whereas a bad
   auto-detection silently mis-parses everything and looks like a firmware bug.
2. **Auto-detect as a first-run convenience.** Candidate scan: configure the UART at a
   candidate's baud, run its descriptor, and require **N consecutive valid checksums** to
   declare a lock; otherwise move to the next candidate after a timeout. The checksum is a
   self-validating oracle, and it reuses A2's parsers rather than adding a timing subsystem.
   A2's cross-protocol negative test is what says whether this can work at all.
3. **Edge-timing baud detection — only if (2) proves too slow.** GPIOTE + TIMER capture over
   PPI hardware-timestamps RX edges at zero CPU cost; the minimum pulse over many edges is one
   bit time, snapped to the nearest standard rate. The trap: the shortest observed pulse is
   only one bit if the data contains an isolated bit, so it needs many samples. It also needs
   the pin as GPIO during detection and handed back to UARTE afterwards. Listed for
   completeness; probably unnecessary.

**Runtime baud changes:** `uart_rx_disable()` → `uart_configure()` → `uart_rx_enable()`.
**⚠ Verify before use:** that `uart_configure()` is supported by the nrfx UARTE driver in
NCS v3.3.1 — this is assumed, not confirmed. Add it to `ADD_TESTING_PLAN.md`'s
verify-before-use list.

**Confidence monitoring (cheap, worth having):** if checksum failures (A3) exceed a threshold
over a window while locked, the selection is probably wrong — log it loudly and, if the choice
came from auto-detect rather than the app, re-run detection. Never silently re-detect a
user-chosen protocol; tell them instead.

**Files:** `src/proto_select.c/.h` (pure: candidate order, lock criteria, confidence policy),
`src/main.c` (the `uart_configure()` glue), `src/control_svc.c` (`SET_PROTOCOL`),
`src/lock_prefs.c` (persistence — same settings handler as B6),
`tests/host/test_proto_select.c`, `tests/integration/proto_select/` (`uart_emul` can be
reconfigured, so the scan is testable on `native_sim`).

**Tests**
- **Pure:** candidate ordering; N-consecutive-valid lock criterion; a stream that never
  validates exhausts candidates and reports failure rather than locking on noise; confidence
  policy triggers at the threshold and not before; a user-set protocol is never silently
  replaced.
- **Integration on `native_sim`:** feed protocol A's capture through `uart_emul`, assert the
  selector locks onto A's descriptor and not another; feed pure noise, assert no lock; feed A
  then switch to B, assert the confidence monitor notices.

**Commit:** `feat(proto): select the controller protocol from the app, with auto-detect fallback`

---

## What this workstream no longer contains

Recorded so the deleted work is not silently re-invented:

- ~~Second UART in devicetree; DK console to RTT~~ — no second serial peer exists.
- ~~`uart_bridge` singleton → `struct uart_link` refactor~~ — one link is all there is. The
  SPSC contract in `uart_bridge.h` stands unchanged and untouched.
- ~~Controller ⇄ display passthrough with BLE fan-out~~ — the app is the display.
- ~~Fail-safe passthrough bypass for a firmware hang~~ — a hang costs the app's view, not the
  bike's ability to run. Worth a watchdog eventually; not a safety-critical bypass.

---

## Open items

- **Q1 — which four protocols?** Names them, and decides how much of A1 is capture versus
  reverse engineering from published sources.
- **Q7** — framed messages to the app, or raw bytes (A2).
- **Verify:** `uart_configure()` support on the nrfx UARTE driver in NCS v3.3.1 (A4).
- **Measure:** actual RAM usage, via `arm-zephyr-eabi-size` on a built ELF. The budget section
  above has flash and CPU numbers but RAM is currently unquantified.
