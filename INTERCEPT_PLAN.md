# INTERCEPT_PLAN.md — Workstream C: interception and insertion

Part of [`FEATURE_PLAN.md`](FEATURE_PLAN.md). Turns the pass-through datapath into something
that can inspect, rewrite, drop and inject messages — and feed extra sensor data into the
stream or up to the phone.

**Depends on:** A1 (`PROTOCOL.md` + capture corpus), A4 (passthrough), **A5 (framing)**.
Nothing here starts before A5 lands; modifying a stream you cannot delimit is how you corrupt
a display.

---

## The safety frame for this workstream

Every commit here writes bytes that a motor controller or a display will act on. Two rules,
applied to every task:

1. **The default configuration is byte-identical pass-through.** The hook layer ships doing
   nothing, and there is a test asserting that a captured stream replayed through the full
   layer comes out bit-for-bit unchanged. Every behaviour is opt-in from there.
2. **Never emit a frame that fails its own checksum.** Enforced as a property test over
   everything the emitter can produce (C3), not as a code-review habit.

What you *choose* to modify is your call; note that nothing in this plan alters assist or speed
behaviour, and anything that does is a road-legality question rather than a firmware one.

---

## C1 — Message-level hook layer

**Why:** `on_uart_rx` / `on_ble_rx` operate on transport chunks. With A5's parser in place,
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
        uint8_t  direction; /* CONTROLLER_TO_DISPLAY / DISPLAY_TO_CONTROLLER */
};

/* Per-message-type handlers; unregistered types are PASS by definition. */
enum intercept_verdict intercept_message(struct intercept_ctx *ctx,
                                         struct intercept_msg *msg);

/* Messages the firmware originates, drained by the emitter between real frames. */
int intercept_queue_insert(struct intercept_ctx *ctx, const uint8_t *frame, size_t len);
```

Insertion is a **queue drained between real messages**, not an interruption of one — splicing
bytes into the middle of a frame in flight is the obvious catastrophic failure and the API
should make it unrepresentable.

**Tests**
- **Identity property:** every capture in `tests/data/` replayed through the layer with no
  handlers registered emerges byte-identical. This is rule 1 above, mechanised.
- a handler returning `DROP` removes exactly that message and leaves neighbours untouched;
- a handler that rewrites a field produces a frame whose checksum validates (via A5's parser —
  parse what you emit, round-trip);
- a handler that grows a message past `cap` is rejected rather than overflowing;
- queued insertions appear **between** messages, never inside one, and preserve ordering;
- an insertion queued while the queue is full is dropped and counted, not blocking.

**Commit:** `feat(intercept): add the message-level interception layer`

---

## C2 — Extra sensor inputs

**Why:** the second half of "intercepting/inserting any extra data/sensors we might need".

**Blocked on Q4** (which sensors, which bus) — but the *structure* is sensor-agnostic and can
be built first.

**Files**
- `src/sensor_src.h`, `src/sensor_src.c` (new, Zephyr-free — the **scheduling and staleness
  policy**, not the driver)
- `src/main.c` — a sampling thread or work item calling the actual Zephyr sensor/ADC/I²C APIs
- `boards/*.overlay` — the sensor nodes, per board
- `tests/host/test_sensor_src.c`

**Design:** keep the pure part pure. `sensor_src` owns "when is a sample due", "is the last
sample stale", "what do we publish when the sensor is unreadable" — all decidable from
`(now_ms, last_sample_ms, interval_ms)` and testable on the host. The Zephyr driver call is a
five-line function in `main.c` that hands the value in.

**Where samples go — two destinations, deliberately different risk:**
- **Up to the phone via telemetry (workstream D): safe, default.** Nothing on the bike sees it.
- **Injected into the display stream as synthetic messages: only for message types A1 proved
  the display tolerates.** If `PROTOCOL.md` does not record how the display reacts to an
  unexpected or extra frame, this path stays off. Do not discover it on a ride.

**Tests:** sampling cadence honoured under jitter; a failed read marks the value stale rather
than publishing a stale number as fresh; staleness clears on the next good read; wraparound at
`UINT32_MAX`.

**Commit:** `feat(sensor): add sensor sampling with staleness policy`

---

## C3 — Emission guards

**Why:** the emitter is the last thing between this firmware and hardware that acts on bytes.

**Files:** `src/frame.c` (an emit/serialise path beside the parser), `src/intercept.c`,
`tests/host/test_frame.c`.

**Guards**
- **Checksum recomputed on every emitted frame**, modified or synthesised. A pass-through frame
  keeps its original bytes verbatim (recomputing an untouched frame is a chance to introduce a
  difference where there was none).
- **Length within the protocol maximum** from `PROTOCOL.md`; over-length is refused and counted.
- **Cadence budget** — insertions must not push the stream past the inter-frame timing A1
  measured. If the protocol is polled request/response, an insertion must never delay a
  response past the display's tolerance; the emitter drops queued insertions rather than
  overrun. Count the drops.
- **Direction sanity** — a message type only valid controller→display is never emitted the
  other way.

**Tests (property-style, over the whole capture corpus)**
- every byte sequence the emitter can produce parses cleanly with a valid checksum — generate
  by fuzzing handler outputs, assert the invariant holds for all of them;
- an untouched message round-trips byte-identically (not merely equivalently);
- with insertions queued at maximum rate, the measured inter-frame gap never violates the
  budget; excess insertions are dropped and counted.

**Commit:** `feat(frame): guard emitted frames on checksum, length and cadence`

---

## C4 — Hardware validation

Not a code commit. Nothing from C goes on a ridden bike before:

- [ ] Default build (no handlers registered) inline for a full ride: display behaves exactly as
      it did with the proxy absent. Compare against a pre-A4 baseline ride.
- [ ] One trivial handler (e.g. observe-only, log a message type) — still no display change.
- [ ] Deliberate corruption test on the bench: feed a malformed frame in and confirm the
      display recovers within one message and the resync counter increments by exactly one.
- [ ] Insertion test on the bench first, wheel off the ground second, ride third — in that
      order, and only for message types `PROTOCOL.md` says the display accepts.
- [ ] Sensor injection (if C2's injection path is used) at the intended rate for 10+ minutes,
      watching for accumulated drift in the display's cadence.

---

## Open items

- **Q4** decides C2's driver work and which overlays change.
- The insertion path (C2, C3) is only as safe as `PROTOCOL.md` §"what happens when a frame is
  malformed or missing" — if A1 could not determine that, treat injection as unvalidated and
  keep sensor data on the telemetry path only.
