# FEATURE_PLAN.md — the e-bike feature programme

Written 2026-07-28. This is the **overarching plan and the checklist we tick off**. It covers
four workstreams; each has its own detail document with the individually-commitable tasks:

| # | Workstream | Detail doc | What it delivers |
|---|-----------|-----------|------------------|
| **A** | Controller protocols: capture, framing, selection | [`PROTOCOL_PLAN.md`](PROTOCOL_PLAN.md) | Know the wire formats (**four of them, different baud rates**); parse; pick the right one at runtime |
| **B** | Controller locking (proximity / button) | [`LOCK_PLAN.md`](LOCK_PLAN.md) | Relay on the motor-enable line, gated on app + connection + unlock |
| **C** | Interception / insertion of extra data & sensors | [`INTERCEPT_PLAN.md`](INTERCEPT_PLAN.md) | Enrich what the app receives; validate what it sends |
| **D** | Remote diagnostics / telemetry to the app | [`TELEMETRY_PLAN.md`](TELEMETRY_PLAN.md) | Health/debug snapshots over BLE, compiled out or idle when unused |

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

## 2. The topology, and what the firmware actually is

```
Motor → Motor Controller → nRF52840 UART1 → nRF52840 BLE (NUS) → Android BLE → Android app
                                                                                (the display)
```

**The Android app is the display.** No physical display unit sits in the serial chain. The
firmware therefore has exactly two endpoints — the motor controller on one side, the phone on
the other — and it translates between two different transports.

That makes it a **serial ⇄ Bluetooth LE bridge**, which is precisely what it already is. The
controller↔nRF link is two wires, full duplex: `uart1` TX and RX, one UART, already wired.

Terminology worth fixing, since the words get used loosely:

- **Bridge / gateway** *(what this is)* — joins two different transports and translates. The
  controller and the phone could never talk directly.
- **Proxy** — an intermediary between two endpoints of the **same** protocol. Not what this
  does; the project name is historical.
- **Tap / sniffer** — passive, listen-only, carries nothing. Also not this: the bridge is
  fully bidirectional, and the downlink actively commands the controller.

**⚠ Correction, 2026-07-28.** The first draft of this plan misread "the ebike display screen"
as a physical display on the bike and built a whole workstream around becoming an intermediary
between controller and display: a second UART, a `uart_bridge` singleton refactor, a passthrough
datapath, and a console move to RTT on the DK. **None of that is needed** — there is no second
serial peer. Workstream A is now just protocol capture and framing. The deleted tasks are listed
at the end of `PROTOCOL_PLAN.md` so they are not silently re-invented.

**The direction that carries risk is the downlink.** App → NUS write → `nus_received` →
`on_ble_rx` → `uart_bridge_send` → `uart1` TX → controller. Those are commands the controller
acts on. The uplink's worst failure is a wrong number on a phone screen; the downlink's is a
malformed frame reaching something that drives a motor. Workstream C treats them differently
for that reason.

## 3. Decisions taken (with the reasoning, so they can be overturned knowingly)

**D1 — The app talks to the proxy over a dedicated GATT service, not in-band on NUS.**
A new *Proxy Control Service* (custom 128-bit UUID) with `command` (write), `state` (read +
notify) and `telemetry` (notify) characteristics. NUS stays the raw controller stream.

*Why:* NUS carries controller traffic in both directions; multiplexing lock commands onto it
would need an escape scheme on a binary stream, and worse, would put app→firmware control bytes
on the same path as app→controller command bytes, where a framing bug sends one as the other.
Separate characteristics also carry their own GATT permissions and their own subscription state,
which is exactly what D3 needs ("zero cost when nobody is listening").

*Note worth having:* unlike NUS, the control service **can** use GATT-level
`BT_GATT_PERM_WRITE_ENCRYPT`. That permission requires security level 2 (encrypted,
unauthenticated), which Just Works pairing *does* satisfy — it is `BT_GATT_PERM_*_AUTHEN`
(level 3, MITM) that Just Works can never satisfy and that made `CONFIG_BT_NUS_AUTHEN`
unusable. So the control channel gets stack-enforced encryption, with the existing
`link_secure` app-level gate as belt and braces.

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
codebase is honest about that everywhere; the framing layer (A2) is what changes it.

## 4. Decisions still needed from you

None of these block starting (each has a default that lets work proceed), but each will
re-shape a task if answered differently:

| # | Question | Status / default |
|---|----------|------------------|
| Q1 | **Which four protocols?** Naming them decides how much of A1 is real capture versus reverse engineering from published sources — and **which of them you can physically capture**, since downlink command support should not ship for a protocol never seen on a wire | Open — A1 is structured as one commit per protocol so they can land as access allows |
| ~~Q2~~ | ~~Two wires or one shared half-duplex?~~ | ✅ **Answered: two wires, full duplex** — one UART, already wired. No second port needed |
| Q3 | **What is the motor-enable line electrically?** (voltage, current, switched high or low, is it the ignition/lock line?) | Open — assume a low-current signal line; drive a MOSFET/SSR, not a coil, from GPIO |
| Q4 | **Which sensors are you adding, and on what bus?** (I²C / ADC / one-wire) | Open — C2 is written against a generic sampled-source interface |
| Q5 | **Is a physical unlock button wanted in addition to the app?** | Open — planned as optional B5, behind a `lock-button` alias |
| ~~Q6~~ | ~~Must the display survive a firmware hang?~~ | ✅ **Moot** — the app is the display, so a hang costs the phone's view, not the bike |
| Q7 | **Should the firmware send the app framed messages, or keep raw bytes?** | Open — recommendation is additive framing, app migrates when it wants (`PROTOCOL_PLAN.md` A2) |

## 5. Order of attack

```
A1 capture (xN) ──► A2 frame.c + descriptors ──► A3 counters ──┬──► C1 ──► C2 ──► C3 ──► C4
                                   │                           │
                                   └──► A4 protocol select ────┤
                                              ▲                │
B1 lock core ──► B2 relay GPIO ──► B3 control svc ──► B4 ──► B5/B6 ──► B7
                                        │
                                        └──► D1 ──► D2 ──► D3 ──► D4 ──► D5
```

A4 needs B3's control service for `SET_PROTOCOL` and B6's settings handler for persistence —
the one place the two independent halves of the programme meet.

Rationale for starting where we start:

1. **A1 first** — a capture exercise doable *today*, with no firmware change, that unblocks
   everything with a message boundary in it.
2. **B1–B4 in parallel** — the headline feature, no dependency on A at all, and B3 (the control
   service) is shared infrastructure that D also needs.
3. **A2 → C** — everything that needs to understand messages rather than bytes.
4. **D last** — it wants B3's service and benefits from A3's counters.

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

### Workstream A — Controller protocols (`PROTOCOL_PLAN.md`)
- [ ] **A1** Capture and document each protocol, both directions → `PROTOCOL.md` + fixtures *(one commit per protocol)*
- [ ] **A2** `frame.c` — pluggable multi-protocol parser: one state machine, one descriptor per protocol
- [ ] **A3** Frame-level health counters, separate from transport drops
- [ ] **A4** Protocol + baud selection: app-configured and authoritative, auto-detect as fallback

### Workstream B — Controller locking (`LOCK_PLAN.md`)
- [ ] **B1** `lock_core.c` — the lock state machine, pure logic, test-first
- [ ] **B2** Relay GPIO glue + `motor-enable` alias per board; locked before anything else at boot
- [ ] **B3** Proxy Control Service — GATT skeleton + command codec
- [ ] **B4** Wire B1↔B3↔B2: app unlock/lock, auto-unlock preference, state notifications
- [ ] **B5** *(optional)* Physical unlock button (`lock-button` alias) with debounce
- [ ] **B6** Preference persistence via settings/NVS *(auto-unlock yes; unlocked state never)*
- [ ] **B7** Hardware bring-up + safety checklist

### Workstream C — Interception / insertion (`INTERCEPT_PLAN.md`)
- [ ] **C1** Message-level hook layer on top of A2 — pass / modify / drop / insert
- [ ] **C2** Extra sensor sampling → enriched uplink
- [ ] **C3** Downlink command validation — the direction the controller acts on
- [ ] **C4** Hardware validation

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
- CI matrix extended with the new invariants (`motor-enable` alias present, telemetry Kconfig
  state per target) — a config regression fails the build, which is this project's historical
  failure mode.
- `PROTOCOL.md` documents the wire format the interception layer relies on.
- `CLAUDE.md` updated: the file list, the new modules, and the new alias/Kconfig requirements.
  Its opening line calls the app a "bidirectional **proxy**" — worth correcting to *bridge*
  while there (§2).
- Hardware checklists in `LOCK_PLAN.md` §B7 and `INTERCEPT_PLAN.md` §C4 signed off on the bike.

## 9. Explicitly out of scope

- **Changing assist limits, speed limits, or anything that alters how the motor behaves on the
  road.** The interception layer is built for display/data purposes; what you put through it is
  your call, but nothing in this plan does that, and road-legality is a question for you rather
  than something the firmware should decide.
- BabbleSim end-to-end BLE tests (already deliberately deferred — `TESTING.md` §5).
- Flash readback protection / APPROTECT (`CLAUDE.md`: not wanted, project is open-sourcing).
- OTA/DFU firmware update. Worth its own plan later; not in these four.

## 10. Findings log

Filled in as tasks land. Record anything that
contradicted an assumption above, so the plan stays honest.

| Date | Task | Finding |
|------|------|---------|
| 2026-07-28 | — | `BT_GATT_PERM_*_ENCRYPT` (level 2) is satisfiable by Just Works, unlike the `*_AUTHEN` perms that made `CONFIG_BT_NUS_AUTHEN` unusable — so the control service can be stack-enforced (§3 D1). |
| 2026-07-28 | Q2 | Controller↔nRF link is two wires, full duplex — one UART's TX/RX pair, already wired. |
| 2026-07-28 | A4 | **UART baud rates are not auto-negotiable** — UART is asynchronous with no handshake, and the nRF52840's UARTE has no hardware auto-baud detect. Selection is app-configured (authoritative) with a candidate-scan auto-detect as fallback, using the checksum as the oracle. |
| 2026-07-28 | A2 | **CPU is not a constraint for packet manipulation**: ~5,500 cycles/byte at 115200 baud, ~533,000 at 1200, against 5–10 cycles/byte for a table-driven CRC. Flash ~232 KB of 1 MB used. The real ceilings are BLE throughput (`TODO.md` I1) and RAM — the latter still unmeasured. |
| 2026-07-28 | **§2** | **The Android app is the display; there is no physical display in the serial chain.** The first draft assumed one and specified a second UART, a `uart_bridge` refactor, a passthrough datapath and a DK console move to RTT. All deleted — the existing one-UART bridge is the correct topology. Workstream A collapsed from 7 tasks to 3. |
