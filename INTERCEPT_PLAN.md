# INTERCEPT_PLAN.md — Workstream C: interception and insertion

Part of [`FEATURE_PLAN.md`](FEATURE_PLAN.md). Uses the message framing from workstream A to
inspect, rewrite, drop and augment traffic in both directions — enriching what the app receives,
and validating what it sends to the controller.

**Depends on:** A1 (`PROTOCOL.md` + capture corpus) and **A2 (framing)**. Nothing here starts
before A2 lands; acting on a message you cannot delimit is how you corrupt a command.

---

## The two directions are not equally risky

```
uplink    controller → uart1 RX → ring → ble_write_thread → on_uart_rx → NUS notify → app
downlink  app → NUS write → nus_received → on_ble_rx → uart_bridge_send → uart1 TX → controller
```

- **Uplink — enrichment.** Parse controller messages, optionally add sensor data, hand the app
  a richer picture than the controller alone provides. Worst case for a bug is a wrong number
  on the phone. This is where most of the value is and where experimentation is cheap.
- **Downlink — commands the controller acts on.** Assist level, walk mode, settings. Worst case
  for a bug is a malformed or unintended command reaching something that drives a motor. This
  direction gets validation rather than creativity.

Both hooks (`on_uart_rx`, `on_ble_rx`) already exist as pass-through stubs in `proxy_core.c`
and both already run in thread context, so there is a correct place to put this logic.

## The safety frame for this workstream

1. **The default configuration is byte-identical pass-through.** The hook layer ships doing
   nothing, and there is a test asserting a captured stream replayed through the full layer
   comes out bit-for-bit unchanged. Every behaviour is opt-in from there.
2. **Never emit a frame that fails its own checksum**, in either direction — enforced as a
   property test over everything the emitter can produce (C3), not as a code-review habit.

What you *choose* to modify is your call; note that nothing in this plan alters assist or speed
behaviour, and anything that does is a road-legality question rather than a firmware one.

---

## C1 — Message-level hook layer

**Why:** `on_uart_rx` / `on_ble_rx` operate on transport chunks. With A2's parser in place,
interception should happen on whole messages, with the chunk plumbing hidden underneath.

**Files**
- `src/intercept.h`, `src/intercept.c` (new, Zephyr-free)
- `src/proxy_core.c` — the existing chunk hooks delegate to the message layer
- `tests/host/test_intercept.c`, `tests/unit/intercept/` (ztest), `run.ps1`, `CMakeLists.txt`

**Shape**

```c
enum intercept_verdict { INTERCEPT_PASS, INTERCEPT_MODIFIED, INTERCEPT_DROP };

struct intercept_msg {
        uint8_t  type;
        uint8_t *data;      /* mutable in place, up to cap */
        size_t   len;
        size_t   cap;
        uint8_t  direction; /* UPLINK (to app) / DOWNLINK (to controller) */
};

/* Per-message-type handlers; unregistered types are PASS by definition. */
enum intercept_verdict intercept_message(struct intercept_ctx *ctx,
                                         struct intercept_msg *msg);

/* Messages the firmware originates, drained between real ones. */
int intercept_queue_insert(struct intercept_ctx *ctx, const uint8_t *frame, size_t len);
```

Insertion is a **queue drained between real messages**, not an interruption of one — splicing
bytes into the middle of a frame in flight is the obvious catastrophic failure, and the API
should make it unrepresentable.

**Tests**
- **Identity property:** every capture in `tests/data/` replayed through the layer with no
  handlers registered emerges byte-identical. This is rule 1 above, mechanised.
- a handler returning `DROP` removes exactly that message and leaves neighbours untouched;
- a handler that rewrites a field produces a frame whose checksum validates (round-trip through
  A2's parser — parse what you emit);
- a handler that grows a message past `cap` is rejected rather than overflowing;
- queued insertions appear **between** messages, never inside one, and preserve ordering;
- an insertion queued while the queue is full is dropped and counted, not blocking;
- direction is honoured: an uplink handler never sees downlink traffic.

**Commit:** `feat(intercept): add the message-level interception layer`

---

## C2 — Extra sensors, enriching the uplink

**Why:** the second half of "intercepting/inserting any extra data/sensors we might need".

**Blocked on Q4** (which sensors, which bus) — but the *structure* is sensor-agnostic and can
be built first.

**Files**
- `src/sensor_src.h`, `src/sensor_src.c` (new, Zephyr-free — the **scheduling and staleness
  policy**, not the driver)
- `src/main.c` — a sampling work item calling the actual Zephyr sensor/ADC/I²C APIs
- `boards/*.overlay` — the sensor nodes, per board
- `tests/host/test_sensor_src.c`

**Design:** keep the pure part pure. `sensor_src` owns "when is a sample due", "is the last
sample stale", and "what do we publish when the sensor is unreadable" — all decidable from
`(now_ms, last_sample_ms, interval_ms)` and testable on the host. The Zephyr driver call is a
five-line function in `main.c` that hands the value in.

**Where the samples go:** up the uplink to the app, as additional messages in the stream (or
via workstream D's telemetry characteristic — see below). **This is low-risk now that the app
is the display**: the only consumer is software you control, so an unfamiliar message type is
a parsing decision on the phone, not a confused piece of hardware on the handlebars.

**Which channel — the uplink or the telemetry characteristic?** Sensor data that the *rider*
sees belongs on the uplink alongside controller data, so the app renders one coherent picture.
Sensor data that only *diagnoses the unit* belongs in workstream D's snapshot. Decide per
sensor once Q4 is answered; the sampling core is the same either way.

**Tests:** sampling cadence honoured under jitter; a failed read marks the value stale rather
than publishing a stale number as fresh; staleness clears on the next good read; wraparound at
`UINT32_MAX`.

**Commit:** `feat(sensor): add sensor sampling with staleness policy`

---

## C3 — Downlink command validation

**Why:** this is the one path where the firmware's output reaches something that drives a
motor. It gets guards; the uplink gets flexibility.

**Files:** `src/frame.c` (an emit/serialise path beside the parser), `src/intercept.c`,
`tests/host/test_frame.c`.

**Guards, applied to everything sent to the controller**
- **Checksum recomputed on every emitted frame**, modified or synthesised. A pass-through frame
  keeps its original bytes verbatim — recomputing an untouched frame is a chance to introduce a
  difference where there was none.
- **Known command types only.** A downlink frame whose type is not in `PROTOCOL.md`'s command
  catalogue is refused and counted, not forwarded hopefully.
- **Field ranges validated** against the catalogue (e.g. assist level within the controller's
  accepted set) — the app is trusted, but a bug in the app should not become a bug in the motor
  controller.
- **Length within the protocol maximum**; over-length refused and counted.
- **Direction sanity** — an uplink-only message type is never emitted downlink.

**Tests (property-style, over the whole capture corpus)**
- every byte sequence the emitter can produce parses cleanly with a valid checksum — generate
  by fuzzing handler outputs and assert the invariant holds for all of them;
- an untouched message round-trips byte-identically (not merely equivalently);
- unknown command types and out-of-range fields are refused, with the counter incremented;
- a fuzzed downlink stream never produces an emitted frame that fails validation.

**Commit:** `feat(frame): validate every command sent to the controller`

---

## C4 — Hardware validation

Not a code commit. Nothing from C goes on a ridden bike before:

- [ ] Default build (no handlers registered) for a full ride: the app shows exactly what it
      showed before. Compare against a pre-C baseline capture.
- [ ] Uplink enrichment enabled: the app renders the added data, and the controller-sourced
      fields are unchanged next to the baseline capture.
- [ ] Deliberate corruption test on the bench: feed a malformed frame into the uplink and
      confirm the parser resyncs within one message and the counter increments by exactly one.
- [ ] **Downlink commands on the bench first, wheel off the ground second, ridden third** — in
      that order, for each command type, checking the controller does what the command says
      and nothing else.
- [ ] Sensor sampling (if C2 lands) at the intended rate for 10+ minutes, watching for drift,
      staleness handling, and any effect on uplink cadence.

---

## Open items

- **Q4** decides C2's driver work and which overlays change.
- **Q7** (framed messages to the app, or raw bytes) affects how C1's output reaches the phone —
  see `PROTOCOL_PLAN.md` A2.
- C3's field-range validation is only as good as `PROTOCOL.md`'s command catalogue. If A1 could
  not establish what the controller does with an out-of-range value, keep the validation
  conservative — refuse anything not observed in a real capture.
