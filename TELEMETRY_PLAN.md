# TELEMETRY_PLAN.md — Workstream D: remote diagnostics / telemetry

Part of [`FEATURE_PLAN.md`](FEATURE_PLAN.md). Streams health and diagnostic data to the app —
**conditional on it costing the datapath nothing measurable**, which is stated as a requirement
and enforced as a test (D4) rather than assumed.

**Depends on:** B3 (the control service provides the `telemetry` characteristic).
**Benefits from:** A3 (frame-level counters) — but D1 can ship with
whatever counters exist at the time and grow.

---

## The performance contract (this shapes every task below)

Three layers of "costs nothing", weakest to strongest:

1. **Compile-time:** `CONFIG_NRFPROXY_TELEMETRY=n` removes the code entirely. Off in
   `prod.conf`; on for debug targets.
2. **Runtime-idle:** compiled in but **nothing runs until the phone subscribes** to the
   `telemetry` characteristic. No sampling timer, no work item, no allocation.
3. **Bounded when active:** one snapshot per second, sampled in a low-priority work item, sent
   with the same `proxy_send_result()` back-off policy the data path already uses — telemetry
   yields to bike traffic, never the reverse.

D4 makes (3) falsifiable: a sustained-burst integration test with telemetry on must show **no
increase in datapath drops** versus telemetry off. If it does, the feature gets pared back —
that is what "optional if there's any significant performance impact" means in practice.

---

## D1 — `telemetry_core.c`: the metrics snapshot

**Why:** decide *what* is worth reporting, in pure logic, before any transport exists.

**Files:** `src/telemetry_core.h`, `src/telemetry_core.c` (new, Zephyr-free),
`tests/host/test_telemetry_core.c`, `run.ps1`, `CMakeLists.txt`.

**Snapshot contents** (grow as the other workstreams land):

| Field | Source | Why it earns its bytes |
|---|---|---|
| uptime_s | kernel | correlates everything else |
| lock_state, lock_reason | `lock_core` (B1) | the app already wants this; free here |
| RX-overflow / TX-fail / TX-abort counters | `drop_stats` (existing) | the silent-loss points `TODO.md` L1 was raised about |
| frame checksum failures, resyncs, over-length rejections | `frame.c` (A3) | wiring/EMI problems show here first |
| downlink commands refused by validation | `intercept` (C3) | an app bug that never reached the controller |
| ring high-water marks | `uart_bridge` | tells you if the buffers are sized right on real traffic |
| ATT MTU, connection interval | BT stack | explains throughput complaints (`TODO.md` I1) |
| sensor staleness flags | `sensor_src` (C2) | is the extra hardware actually alive |

**Design:** the module owns a snapshot struct and the *policy* — sampling interval, which
counters are deltas versus absolutes, and what "changed enough to be worth sending" means.
Counters come in as plain integers; nothing Zephyr-shaped crosses the boundary.

**Tests:** snapshot assembly from synthetic inputs; delta computation across successive
snapshots including counter wraparound; the "worth sending" predicate (an idle system with
nothing changing should not generate traffic every second).

**Commit:** `feat(telemetry): add the diagnostics snapshot core`

---

## D2 — Versioned compact encoding

**Why:** the app has to parse this, and firmware and app will drift in version.

**Files:** `src/telemetry_enc.c/.h` (or inside `telemetry_core.c`), `tests/host/test_telemetry_enc.c`.

**Format:** leading version byte, then TLV — `[tag][len][value]`. TLV rather than a packed
struct so an older app skips unknown tags instead of misparsing, and a newer field costs
nothing to add. Whole snapshot must fit one notification at the negotiated MTU (≤ 244 bytes);
if it does not, split by tag across successive notifications rather than fragmenting a value.

**Tests:** encode/decode round-trip for every field; buffer exactly-full and one-byte-short
cases; unknown tag is skipped by the decoder using its length; truncated TLV is rejected;
version mismatch surfaces as an error rather than garbage; **fuzz the decoder** over random
byte strings and assert it never reads past the buffer (this decoder will one day be fed by
something other than us).

**Commit:** `feat(telemetry): add versioned TLV encoding for snapshots`

---

## D3 — Transport: notify when subscribed, idle otherwise

**Files:** `src/control_svc.c` (the `telemetry` characteristic + its CCC callback),
`src/main.c` (a delayed work item, started on subscribe and **cancelled on unsubscribe or
disconnect**), `tests/host/test_telemetry_core.c` (the gating policy).

**Requirements**
- The CCC-changed callback is the only thing that starts sampling. No subscription → the work
  item is never scheduled (layer 2 of the performance contract).
- Send with `proxy_send_result()`; on `PROXY_SEND_RETRY` **skip this snapshot entirely** rather
  than retrying. Telemetry is the most droppable data in the system — the next one is a second
  away, and buffer pressure means the bike traffic needs the buffers.
- Sample in a **low-priority work item**, never in the pump threads or the BT RX thread.
- Cancel on disconnect, alongside the existing `security_timeout_work` cancellation.

**Tests:** the gating predicate is pure and host-tested (subscribed + secure + interval elapsed
→ send; any missing → no work). The GATT/CCC plumbing is glue, verified on hardware.

**Commit:** `feat(telemetry): notify snapshots only while the app is subscribed`

---

## D4 — The performance regression test *(the task that makes the feature conditional)*

**Why:** this is the user's actual acceptance criterion, turned into something CI can fail.

**Files:** `tests/integration/uart_bridge/` extended, or a new `tests/integration/telemetry_perf/`.

**The test** on `native_sim` with emulated UARTs:
1. Drive a sustained burst through the uplink datapath with telemetry **compiled out**;
   record datapath drop counters and the message count that made it through intact.
2. Repeat with telemetry compiled in but **unsubscribed** — must be identical (layer 2).
3. Repeat with telemetry **subscribed and notifying at 1 Hz** — datapath drops must not exceed
   the baseline, and no message may be lost that survived in step 1.

Assert on the numbers, not on impressions. If step 3 regresses, the answer is to reduce the
telemetry rate or snapshot size until it does not — the feature is explicitly optional and
loses the argument against the data path every time.

**Also worth capturing once** (manually, not in CI): current draw with telemetry active on the
XIAO, since anything that keeps the radio busier costs battery. Record it in the findings log.

**Commit:** `test(telemetry): assert telemetry never costs the datapath`

---

## D5 — Kconfig, `prod.conf`, and config assertions

**Files:** `Kconfig` (new at repo root, or `Kconfig.nrfproxy`), `prj.conf`, `prod.conf`,
`scripts/check_configs.py`, `scripts/test_check_configs.py`, `README.md`, `CLAUDE.md`.

- `CONFIG_NRFPROXY_TELEMETRY` — default `y`, set `n` in `prod.conf`.
- `check_configs.py` gains a check (`A13`, after the lock's `A12` in `LOCK_PLAN.md` B2):
  telemetry **off** for `xiao_prod` and
  `promicro_prod`, **on** for the four debug targets. This project's failure mode is config
  drift, so it gets the same treatment as every other invariant.
- `prod.conf`'s existing guarantees are unaffected — it must still keep `BT_SMP`, `BT_SETTINGS`,
  `NVS`, `SERIAL` and the async UART instances (the existing `A9` check already asserts this;
  make sure the new fragment does not disturb it).

**Tests:** `scripts/test_check_configs.py` fixtures for both polarities; the six-target build
matrix proves both configurations compile.

**Commit:** `feat(telemetry): gate telemetry behind Kconfig and assert it per target`

---

## Open items

- Whether the app wants telemetry **streamed** (1 Hz notifications) or **polled** (read the
  characteristic on demand). Polled is strictly cheaper and covers the "remote diagnostics"
  use case; streamed is better for live graphs. **Recommendation: build the notify path with
  a readable `state`, and let the app choose by subscribing or not** — which is what D3 already
  does, so this is a documentation question rather than a design one.
- Log capture over BLE (shipping `LOG_*` output to the app) is *not* in this plan. It is a much
  bigger hammer — `CONFIG_LOG_BACKEND` work, buffering, and real throughput cost — and the
  counter snapshot answers most diagnostic questions for a fraction of the price. Revisit only
  if the snapshots prove insufficient.
