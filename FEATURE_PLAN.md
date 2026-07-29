# FEATURE_PLAN.md — the e-bike feature programme

Written 2026-07-28. This is the **overarching plan and the checklist we tick off**. It covers
four workstreams; each has its own detail document with the individually-commitable tasks:

| # | Workstream | Detail doc | What it delivers |
|---|-----------|-----------|------------------|
| **A** | Controller ⇄ display man-in-the-middle datapath | [`MITM_PLAN.md`](MITM_PLAN.md) | The bridge actually sits *in* the serial link, not beside it; message framing |
| **B** | Controller locking (proximity / button) | [`LOCK_PLAN.md`](LOCK_PLAN.md) | Relay on the motor-enable line, gated on app + connection + unlock |
| **C** | Interception / insertion of extra data & sensors | [`INTERCEPT_PLAN.md`](INTERCEPT_PLAN.md) | Message-level rewrite/inject, extra sensor inputs |
| **D** | Remote diagnostics / telemetry to the app | [`TELEMETRY_PLAN.md`](TELEMETRY_PLAN.md) | Health/debug snapshots over BLE, compiled out or idle when unused |

Workstream A was not on the spoken list of three, but it is the load-bearing one: **the
firmware as built today is a one-UART tap (UART1 ⇄ BLE), and three of the four features
assume it sits between the controller and the display.** See §2. If you only wanted a tap,
say so and A shrinks to A1 + A5.

---

## 1. Ground rules (inherited — binding)

From `CLAUDE.md` and the existing plans; nothing below may violate these:

- **`proxy_core.c` has no Zephyr dependency of any kind.** All new *decision* logic goes into
  equally Zephyr-free modules (`lock_core.c`, `frame.c`, `telemetry_core.c`), so it is testable
  on a host with plain gcc. `main.c` stays glue: it owns every Zephyr/BT/GPIO call, all
  locking, and all callbacks.
- **`main.c` BUILD_ASSERTs the couplings** the pure modules deliberately can't see (buffer
  sizes, address lengths, Kconfig limits). Add one for every new boundary constant.
- **Git is the user's job.** Every task below is sized to one commit, with a suggested message,
  but the commits are yours to make.
- **Windows dev box, Linux CI.** `native_sim` is Linux-only, so the fast inner loop on Windows
  is `tests/host/run.ps1` (plain gcc) plus the six-config build matrix. ztest suites land in CI.
- **No security hardening beyond what's asked** (no APPROTECT/readback protection) — firmware
  is being open-sourced.
- **Per-board everything.** Four boards (DK, XIAO, Pro Micro, Dongle); anything pin-shaped goes
  in `boards/<target>.overlay`, anything Kconfig-shaped that isn't universal goes in
  `boards/<target>.conf`. Filenames must match the *full* normalised board target.

## 2. The architectural finding that shapes the ordering

**Today:** `uart1` ⇄ ring buffers ⇄ BLE NUS. One serial port. The proxy is a *tap or an
endpoint*, not an intermediary.

**What features A/C need:** controller TX → proxy RX, proxy TX → display RX, and the reverse
for the display's replies. That is **two UARTs**, and the nRF52840 has exactly two UARTE
instances (UARTE0, UARTE1) — so once both carry bike traffic, **the console cannot live on a
UART on any board**. Consequences, all handled in `MITM_PLAN.md`:

- XIAO / Pro Micro / Dongle already put the console on USB CDC-ACM — unaffected.
- The **DK loses its J-Link VCOM console** and moves to **RTT** (`CONFIG_USE_SEGGER_RTT` +
  `CONFIG_LOG_BACKEND_RTT`). This is a one-task change (A2) but it changes how you debug on
  the DK, so it is called out here rather than buried.
- `uart_bridge.c` is currently a **singleton** — file-scope rings, slab, and in-progress flag.
  It has to become an instantiable `struct uart_link` before a second port exists (A3). This
  is the single largest refactor in the programme and it is behaviour-preserving, so it gets
  its own commit and rides on the existing `tests/integration/uart_bridge` suite.

**Workstream B (locking) does not depend on any of this** — it is GPIO plus a state machine
plus a control channel. That is why B starts first in the ordering below: it is the feature
with the most user-visible value and the fewest prerequisites.

## 3. Decisions taken (with the reasoning, so they can be overturned knowingly)

**D1 — The app talks to the proxy over a dedicated GATT service, not in-band on NUS.**
A new *Proxy Control Service* (custom 128-bit UUID) with `command` (write), `state` (read +
notify) and `telemetry` (notify) characteristics. NUS stays the raw, transparent stream.

*Why:* the controller stream is arbitrary binary, so any in-band command escape needs framing
and an escape/stuffing scheme on a stream we do not yet fully understand — a bug factory.
Separate characteristics also let each one carry its own GATT permissions and its own
subscription state, which is exactly what D3 needs ("zero cost when nobody is listening").

*Note worth having:* unlike NUS, the control service **can** use GATT-level
`BT_GATT_PERM_WRITE_ENCRYPT`. That permission requires security level 2 (encrypted,
unauthenticated), which Just Works pairing *does* satisfy — it is `BT_GATT_PERM_*_AUTHEN`
(level 3, MITM) that Just Works can never satisfy and that made `CONFIG_BT_NUS_AUTHEN`
unusable. So the control channel gets stack-enforced encryption, with the existing
`link_secure` app-level gate as belt and braces.

*The cheaper alternative, if app churn is the constraint:* command framing on the **phone→
device direction only** is safe (that direction is entirely app-originated, so there is no
foreign binary to collide with). The uplink would still need tagging for telemetry. Take this
only if adding a service to the app is expensive.

**D2 — Fail-safe is "de-energised = motor disabled", and boot is always locked.**
A dead board, flat battery, or crashed firmware leaves the motor disabled rather than
permanently enabled. Unlocked state is **never persisted** — that would defeat the lock across
a power cycle, which is the theft case.

*The rider-safety counterweight, which the state machine encodes:* **a BLE dropout alone must
never re-lock.** Once unlocked, stay unlocked until an explicit lock command, a power cycle,
or a long stationary-inactivity timeout. Cutting assist mid-ride because a phone's radio
hiccupped is the failure mode to design against. See `LOCK_PLAN.md` §B1 for the full table.

**D3 — Telemetry is compile-time optional and runtime-idle.**
`CONFIG_NRFPROXY_TELEMETRY` (default `y` on debug targets, `n` in `prod.conf`), and even when
compiled in it does nothing until the phone subscribes. The user's condition — "optional if
there's any significant performance impact" — is turned into an actual regression test in D4
rather than an assurance.

**D4 — Message framing is discovered, not assumed.**
Nothing in C or D that needs message boundaries starts before A1 has captured and documented
the real wire protocol in `PROTOCOL.md`. The hooks today see *transport chunks* and the
codebase is honest about that everywhere; the framing layer (A5) is what changes it.

## 4. Decisions still needed from you

None of these block starting (each has a default that lets work proceed), but each will
re-shape a task if answered differently:

| # | Question | Default if unanswered |
|---|----------|----------------------|
| Q1 | **Which controller/display protocol?** (Bafang UART, KT/Kunteng, APT, proprietary…) | A1 captures it blind and documents what it finds |
| Q2 | **Is the controller↔display link two wires (full duplex) or one shared half-duplex wire?** | Assume full duplex, two UARTs (A2/A3) |
| Q3 | **What is the motor-enable line electrically?** (voltage, current, switched high or low, is it the ignition/lock line?) | Assume a low-current signal line; drive a MOSFET/SSR, not a coil, from GPIO |
| Q4 | **Which sensors are you adding, and on what bus?** (I²C / ADC / one-wire) | C2 is written against a generic sampled-source interface |
| Q5 | **Is a physical unlock button wanted in addition to the app?** | Planned as optional B5, behind a `lock-button` alias |
| Q6 | **Does the display need to keep working if the proxy firmware hangs?** | A7 (hardware bypass) is listed but not scheduled |

## 5. Order of attack

```
A1 capture protocol ─────────────┬─────────────────────────► A5 framing ──► C1 ──► C2 ──► C3
                                 │                              ▲
B1 lock core ──► B2 relay GPIO ──┤                              │
                                 ├─► B3 control service ──► B4 ─┤
                                 │        │                     │
                                 │        └──────────────► D1 ──┴─► D2 ──► D3 ──► D4 ──► D5
                                 │
A2 second UART ──► A3 uart_link ──► A4 passthrough ─────────────┘
```

Rationale for starting where we start:

1. **A1 first** — it is a capture exercise doable *today* with the firmware as it stands, it
   costs almost nothing, and it unblocks everything with a message boundary in it.
2. **B1–B3 next** — the headline feature, no dependency on the MITM refactor, and B3 (the
   control service) is shared infrastructure that D also needs.
3. **A2–A4 then** — the invasive refactor, done when there is a reason for it and with the
   existing integration suite as the safety net.
4. **A5 → C → D last** — everything that needs to understand messages rather than bytes.

## 6. Test strategy — which tier catches what

The repo already has four tiers; every task below names the ones it uses.

| Tier | Where | Runs on | Good for |
|------|-------|---------|----------|
| **Host** | `tests/host/*.c` + `run.ps1` | Windows dev box, plain gcc | Pure logic, the fast inner loop — lock state machine, frame codec, telemetry encoding |
| **Unit (ztest)** | `tests/unit/<name>/` | `native_sim`, CI | The same logic under the Zephyr build, plus anything needing ztest fixtures |
| **Integration (ztest)** | `tests/integration/<name>/` | `native_sim`, CI | Real drivers, emulated hardware: `uart_emul` (already used), `gpio_emul` (new, for the relay) |
| **Config** | `scripts/check_configs.py` | Windows + CI, all six targets | Build invariants — offsets, async gating, prod.conf strippings, new alias/Kconfig requirements |
| **Hardware** | Manual checklists in each plan | The bike | Anything none of the above can reach: relay switching under load, real protocol timing, pairing UX |

**Every new pure module gets a host test in the same commit that introduces it.** That is the
rule that keeps this programme testable, and it is cheap because the modules are Zephyr-free by
construction.

## 7. Master checklist

Tick these off as they land. Task detail — files touched, tests, acceptance criteria, suggested
commit message — is in the per-workstream docs.

### Workstream A — MITM datapath (`MITM_PLAN.md`)
- [ ] **A1** Capture and document the controller↔display protocol → `PROTOCOL.md`
- [ ] **A2** Second UART in devicetree; console off UART on every board (DK → RTT)
- [ ] **A3** Refactor `uart_bridge` singleton → instantiable `struct uart_link` *(behaviour-preserving)*
- [ ] **A4** Wire the two links through: controller ⇄ display passthrough, BLE gets a copy
- [ ] **A5** `frame.c` — message framing/reassembly with resync and checksum validation
- [ ] **A6** Per-link drop accounting and health counters
- [ ] **A7** *(optional, hardware)* Fail-safe passthrough bypass so a firmware hang doesn't kill the display

### Workstream B — Controller locking (`LOCK_PLAN.md`)
- [ ] **B1** `lock_core.c` — the lock state machine, pure logic, test-first
- [ ] **B2** Relay GPIO glue + `motor-enable` alias per board; locked before anything else at boot
- [ ] **B3** Proxy Control Service — GATT skeleton + command codec
- [ ] **B4** Wire B1↔B3↔B2: app unlock/lock, auto-unlock preference, state notifications
- [ ] **B5** *(optional)* Physical unlock button (`lock-button` alias) with debounce
- [ ] **B6** Preference persistence via settings/NVS *(auto-unlock yes; unlocked state never)*
- [ ] **B7** Hardware bring-up + safety checklist

### Workstream C — Interception / insertion (`INTERCEPT_PLAN.md`)
- [ ] **C1** Message-level hook layer on top of A5 — pass / modify / drop / insert
- [ ] **C2** Extra sensor sampling → injected messages
- [ ] **C3** Emission guards — checksum recompute, cadence and length budgets
- [ ] **C4** Hardware validation against the real display

### Workstream D — Telemetry (`TELEMETRY_PLAN.md`)
- [ ] **D1** `telemetry_core.c` — the metrics snapshot, pure logic
- [ ] **D2** Versioned compact encoding
- [ ] **D3** Transport: notify on subscribe, idle otherwise
- [ ] **D4** Performance regression test — telemetry on must not cost the datapath
- [ ] **D5** `CONFIG_NRFPROXY_TELEMETRY` + `prod.conf` handling + config assertions

## 8. Definition of done (whole programme)

- All six build targets green, `python scripts/check_configs.py` passes for each.
- `tests/host/run.ps1` passes on the Windows box; `west twister -T tests/unit` and
  `-T tests/integration` pass on `native_sim` in CI.
- CI matrix extended with the new invariants (second UART async, `motor-enable` alias present,
  telemetry Kconfig state per target) — a config regression fails the build, which is this
  project's historical failure mode.
- `PROTOCOL.md` documents the wire format the interception layer relies on.
- `CLAUDE.md` updated: the file list, the two-UART architecture, the console change on the DK,
  and the new alias/Kconfig requirements.
- Hardware checklists in `LOCK_PLAN.md` §B7 and `INTERCEPT_PLAN.md` §C4 signed off on the bike.

## 9. Explicitly out of scope

- **Changing assist limits, speed limits, or anything that alters how the motor behaves on the
  road.** The interception layer is built for display/data purposes; what you put through it is
  your call, but nothing in this plan does that, and road-legality is a question for you rather
  than something the firmware should decide.
- BabbleSim end-to-end BLE tests (already deliberately deferred — `ADD_TESTING_PLAN.md` Phase 5).
- Flash readback protection / APPROTECT (`CLAUDE.md`: not wanted, project is open-sourcing).
- OTA/DFU firmware update. Worth its own plan later; not in these four.

## 10. Findings log

Filled in as tasks land — same convention as `ADD_TESTING_PLAN.md`. Record anything that
contradicted an assumption above, so the plan stays honest.

| Date | Task | Finding |
|------|------|---------|
| 2026-07-28 | — | Plan written. nRF52840 has exactly two UARTE instances, so the DK's UART console must move to RTT once both carry bike traffic (§2). |
| 2026-07-28 | — | `BT_GATT_PERM_*_ENCRYPT` (level 2) is satisfiable by Just Works, unlike the `*_AUTHEN` perms that made `CONFIG_BT_NUS_AUTHEN` unusable — so the control service can be stack-enforced (§3 D1). |
