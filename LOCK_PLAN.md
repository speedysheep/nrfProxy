# LOCK_PLAN.md — Workstream B: controller locking

Part of [`FEATURE_PLAN.md`](FEATURE_PLAN.md). Delivers: a relay in the motor-enable line,
released only when the owner's phone is connected, encrypted, and has either sent an unlock
command or has auto-unlock enabled as a preference.

**Depends on:** nothing in workstream A. This can start immediately.
**Feeds:** B3 (the control service) is shared infrastructure that workstream D also uses.

---

## Design summary

```
                 ┌───────────────┐
  BLE link  ───► │               │
  app cmds  ───► │  lock_core.c  │ ───► relay enable (bool) ───► lock_relay.c ───► GPIO ───► MOSFET/SSR ───► motor-enable line
  button    ───► │  (pure logic) │ ───► status + reason ──────► control service notify
  time (ms) ───► │               │                            └► optional LED
                 └───────────────┘
```

`lock_core.c` is Zephyr-free — same binding rule as `proxy_core.c`. It takes an input struct
and a monotonic millisecond timestamp, and returns the relay state plus a status/reason. Every
transition below is a host test before any GPIO exists.

### The state table (this *is* the specification — B1 implements it)

States: `LOCKED`, `UNLOCKED`, `UNLOCKED_GRACE` (unlocked but the phone has gone away).

| From | Event / condition | To | Relay |
|------|------------------|----|-------|
| — | boot / reset | `LOCKED` | off |
| `LOCKED` | unlock command, link connected **and** secure | `UNLOCKED` | **on** |
| `LOCKED` | unlock command, link not secure | `LOCKED` | off (reject, reason `NOT_SECURE`) |
| `LOCKED` | link becomes secure **and** `auto_unlock` preference set | `UNLOCKED` | **on** |
| `LOCKED` | button press *(B5)*, link connected and secure | `UNLOCKED` | **on** |
| `LOCKED` | button press, no secure link | `LOCKED` | off (reason `NO_PHONE`) |
| `UNLOCKED` | explicit lock command | `LOCKED` | off |
| `UNLOCKED` | button long-press *(B5)* | `LOCKED` | off |
| `UNLOCKED` | BLE disconnect | `UNLOCKED_GRACE` (deadline `now + RELOCK_GRACE_MS`) | **stays on** |
| `UNLOCKED_GRACE` | reconnect + secure before deadline | `UNLOCKED` | on (deadline cleared) |
| `UNLOCKED_GRACE` | deadline reached **and** known stationary | `LOCKED` | off |
| `UNLOCKED_GRACE` | deadline reached, motion unknown | `LOCKED` after `RELOCK_HARD_MS` | off |
| any | power loss | `LOCKED` on next boot | off |

**The two rules that must not be quietly optimised away:**

1. **A BLE dropout never re-locks immediately.** Radios hiccup; cutting assist under a rider
   mid-hill because a phone went into a pocket is the failure mode this grace state exists for.
   `RELOCK_GRACE_MS` defaults to **10 minutes**, `RELOCK_HARD_MS` to **30 minutes**.
2. **Unlocked is never persisted.** `LOCKED` is the boot state unconditionally — that is the
   entire theft story. Only the *preference* (`auto_unlock`) persists (B6).

"Known stationary" needs a speed signal, which only exists once workstream C can read the
controller stream. Until then `motion_known` is always false and only the hard timeout applies.
The input struct carries the field from day one so C plugs in without touching the state table.

### Hardware note (needs Q3 from `FEATURE_PLAN.md` §4)

- **A GPIO cannot drive a relay coil directly** (nRF52840 pins are ~14 mA max, standard coils
  want 30–80 mA). Drive a logic-level MOSFET or a solid-state relay; if it is a mechanical
  relay, a flyback diode across the coil is not optional.
- **Prefer a latching (bistable) relay or an SSR/MOSFET** on a battery-powered unit — a held
  coil burns tens of milliamps continuously, which dwarfs the entire power budget this firmware
  was tuned for (sub-100 µA advertising, `prod.conf`, DC/DC, two-phase advertising).
- If the motor-enable line is a **low-current signal** (many controllers expose the
  ignition/lock line at battery voltage but only milliamps), a single MOSFET is the whole
  circuit and the power problem disappears.
- **De-energised must equal disabled** (`FEATURE_PLAN.md` D2). Confirm the polarity on the
  bench before it ever sees the bike, and encode it in devicetree (`GPIO_ACTIVE_HIGH`/`_LOW`)
  so the firmware only ever speaks in logical terms.

---

## B1 — `lock_core.c`: the state machine *(pure logic, test-first)*

**Why first:** it is the whole feature's correctness surface, and it is testable on the Windows
box in seconds with no SDK, no board, and no relay wired to anything dangerous.

**Files**
- `src/lock_core.h`, `src/lock_core.c` (new; no Zephyr — the `proxy_core.h` binding rule verbatim)
- `tests/host/test_lock_core.c` (new), `tests/host/run.ps1` (register it)
- `CMakeLists.txt` (add `src/lock_core.c`)

**Interface sketch**

```c
enum lock_state  { LOCK_STATE_LOCKED, LOCK_STATE_UNLOCKED, LOCK_STATE_UNLOCKED_GRACE };
enum lock_reason { LOCK_REASON_BOOT, LOCK_REASON_COMMAND, LOCK_REASON_AUTO,
                   LOCK_REASON_BUTTON, LOCK_REASON_NOT_SECURE, LOCK_REASON_NO_PHONE,
                   LOCK_REASON_GRACE_EXPIRED };

struct lock_inputs {
        bool     connected;       /* BLE link up                       */
        bool     link_secure;     /* security level >= 2               */
        bool     auto_unlock;     /* persisted preference (B6)         */
        bool     motion_known;    /* false until workstream C provides */
        bool     moving;          /* only meaningful if motion_known   */
        uint8_t  command;         /* LOCK_CMD_NONE / _UNLOCK / _LOCK   */
        uint8_t  button;          /* LOCK_BTN_NONE / _SHORT / _LONG    */
        uint32_t now_ms;          /* monotonic                         */
};

struct lock_output {
        enum lock_state  state;
        enum lock_reason reason;
        bool             relay_enable;   /* the only thing that touches hardware */
        bool             changed;        /* true when state != previous          */
        uint32_t         next_deadline_ms; /* 0 = no timer armed                 */
};

void lock_core_init(struct lock_ctx *ctx);                 /* -> LOCKED, relay off */
void lock_core_step(struct lock_ctx *ctx, const struct lock_inputs *in,
                    struct lock_output *out);
```

`next_deadline_ms` is returned rather than the module owning a timer — `main.c` arms a
`k_work_delayable` from it, and the tests drive time by hand. Same trick as
`proxy_security_window_ms()`: milliseconds cross the boundary, not `k_timeout_t`.

**Tests** (`tests/host/test_lock_core.c` — every row of the table above, plus)
- boot state is `LOCKED` with `relay_enable == false`, before any input is stepped;
- unlock rejected when `connected && !link_secure`, and when `!connected`;
- auto-unlock fires exactly once on the secure edge, not repeatedly;
- disconnect from `UNLOCKED` → `UNLOCKED_GRACE` with the relay **still on** and a deadline armed;
- reconnect inside the grace window clears the deadline; reconnect *after* it stays locked;
- `motion_known && moving` blocks the grace-expiry relock until stationary or `RELOCK_HARD_MS`;
- monotonic-clock wraparound at `UINT32_MAX` does not cause a spurious relock (use unsigned
  difference comparison, and test at the boundary);
- idempotence: stepping with identical inputs twice reports `changed == false` the second time.

**Acceptance:** `powershell -File tests/host/run.ps1` passes on Windows with the new suite; the
firmware still builds for all six targets (the module is compiled in but not yet called).

**Commit:** `feat(lock): add the pure lock state machine with host tests`

---

## B2 — Relay output: `lock_relay.c` + `motor-enable` alias *(glue, emulator-tested)*

**Why separate from B1:** this is the only code that touches a pin; keeping it a thin,
independently-testable shim is what lets a `gpio_emul` test prove the boot-locked guarantee
without a bike attached.

**Files**
- `src/lock_relay.h`, `src/lock_relay.c` (new — `gpio_dt_spec` in, logical on/off out)
- `boards/*.overlay` × 4 (a `motor-enable` alias + the chosen pin per board)
- `src/main.c` (call `lock_relay_init()` **before** `bt_enable()`, in fact before anything that
  can fail — the pin must be driven inactive as the first hardware action of `main()`)
- `tests/integration/lock_relay/` (new ztest suite: `CMakeLists.txt`, `prj.conf`,
  `app.overlay`, `src/main.c`, `testcase.yaml`)
- `scripts/check_configs.py` + `scripts/test_check_configs.py` (assert the alias resolves on
  every board — a missing alias silently gives a dark relay, exactly like the LED `_GET_OR`
  fallback)

**Design notes**
- Use `GPIO_DT_SPEC_GET_OR(DT_ALIAS(motor_enable), gpios, {0})` for consistency with the LED
  and `bond-reset` handling, **but unlike those, a missing alias is an error, not a no-op** —
  log it loudly and hold the lock state at `LOCKED`. A board that cannot disable the motor
  should not silently pretend the lock works.
- Configure as `GPIO_OUTPUT_INACTIVE` at init so the pin is driven to "disabled" before the
  first instruction that could fault.
- Polarity lives in devicetree; `lock_relay_set(true)` always means *motor enabled*.

**Tests**
- **New `tests/integration/lock_relay` on `native_sim`** with a `gpio_emul` node in
  `app.overlay` aliased as `motor-enable`:
  - after `lock_relay_init()` the emulated pin reads **inactive**, before anything else runs;
  - `lock_relay_set(true)` / `(false)` drive the logical level, verified through
    `gpio_emul_output_get()` for both `GPIO_ACTIVE_HIGH` and `GPIO_ACTIVE_LOW` variants of the
    overlay (two testcases, or one with two nodes) — this is what catches a polarity inversion;
  - re-init after a set returns the pin to inactive.
- **Config:** `check_configs.py` gains an `A12` check — every target's devicetree resolves
  `motor-enable`; `test_check_configs.py` gains the matching positive/negative fixture.
- Build matrix stays green on all six targets.

**Acceptance:** the gpio_emul suite passes in CI; `check_configs.py` fails loudly if a board
overlay forgets the alias.

**Commit:** `feat(lock): drive a motor-enable relay from devicetree, locked at boot`

---

## B3 — Proxy Control Service: GATT skeleton + command codec

**Why:** the app needs a way to say "unlock" that is not the raw controller stream
(`FEATURE_PLAN.md` D1). Workstream D reuses this service for telemetry, so it is built once,
generically, here.

**Files**
- `src/control_svc.h`, `src/control_svc.c` (new — the GATT service; Zephyr/BT glue)
- `src/lock_cmd.h`, `src/lock_cmd.c` (new — the **pure** command/response codec)
- `tests/host/test_lock_cmd.c` (new) + `run.ps1`
- `prj.conf` (nothing new expected — verify `CONFIG_BT_GATT_DYNAMIC_DB` is not needed for a
  statically-defined service; `BT_GATT_SERVICE_DEFINE` is static)
- `CMakeLists.txt`

**Service shape**

| Characteristic | Props | Perms | Payload |
|---|---|---|---|
| `command` | write | `BT_GATT_PERM_WRITE_ENCRYPT` | `[version][opcode][args…]` |
| `state` | read, notify | `BT_GATT_PERM_READ_ENCRYPT` | `[version][lock_state][reason][flags]` |
| `telemetry` | notify | `BT_GATT_PERM_READ_ENCRYPT` | reserved for workstream D |

Opcodes: `0x01 UNLOCK`, `0x02 LOCK`, `0x03 SET_AUTO_UNLOCK(bool)`, `0x04 GET_STATE`. A leading
version byte so the app and firmware can disagree gracefully; unknown opcode or wrong version
→ rejected with an error response, never silently ignored.

`BT_GATT_PERM_*_ENCRYPT` requires security level 2, which Just Works pairing satisfies — see
`FEATURE_PLAN.md` §3 D1 for why this differs from the `CONFIG_BT_NUS_AUTHEN` trap. Keep the
existing app-level `link_secure` check as well; it costs nothing and it is the gate the rest of
the firmware already speaks.

**Replay/authentication note:** the link is encrypted and the peer is the single bonded phone
(`CONFIG_BT_MAX_PAIRED=1` + filter accept list), so a command replay would have to come from
inside an established encrypted session with the owner's phone. No nonce/counter is warranted;
this sentence exists so the decision is on the record rather than an oversight.

**Tests** (`tests/host/test_lock_cmd.c`)
- every valid opcode parses to the expected struct; argument bounds checked;
- truncated writes (0 bytes, 1 byte, opcode with a missing argument) are rejected, not
  under-read — fuzz the length space from 0 to max;
- unknown version and unknown opcode both produce the error response;
- oversized writes are clamped/rejected without touching memory past the buffer;
- response encoding round-trips against the decoder.

The GATT registration itself is glue and gets verified on hardware (B7) — pure codec logic is
where the bugs would be, and that is fully covered off-target.

**Acceptance:** host tests pass; the service appears in a scan on hardware with the right
permissions (writes from an unencrypted link are rejected by the stack).

**Commit:** `feat(ble): add the proxy control service and its command codec`

---

## B4 — Wire it together: app ⇄ lock core ⇄ relay

**Why:** B1–B3 are three isolated pieces; this is the commit that makes the feature exist.

**Files**
- `src/main.c` — own a `lock_ctx`, step it from the events that matter (connect, disconnect,
  security change, control-service write, timer expiry), arm a `k_work_delayable` from
  `next_deadline_ms`, call `lock_relay_set()` on `changed`, and notify `state`.
- `src/control_svc.c` — the write handler calls into the lock core.

**Concurrency requirements** (this project's bug history is entirely concurrency and config, so
these are requirements, not suggestions):
- The `lock_ctx` is touched from the BT RX thread (writes, connect/disconnect), the system
  workqueue (deadline expiry) and possibly a button ISR/work item. **Guard it with its own
  mutex** — do not extend `conn_mutex`'s remit; it already guards `current_conn`, `link_secure`
  and `adv_active`, and widening a lock's scope is how deadlocks arrive.
- Never call `lock_core_step()` from an ISR. The button (B5) submits a work item, matching how
  `on_uart_rx` was deliberately moved out of ISR context.
- Read `link_secure` under `conn_mutex`, copy it into `struct lock_inputs`, then release before
  stepping — the same snapshot-then-decide pattern `proxy_should_start_adv()` already uses.

**Optional LED:** the lock state is worth showing, but `TODO.md` L3 records that
`set_status_leds()`/`current_status` already have benign races across three contexts. Either
fold lock indication into a proper LED state machine rework, or leave it out of this commit.
**Recommendation: leave it out**, and let the app's `state` notification be the indication for
now. Note it in `TODO.md` rather than growing L3.

**Tests**
- Extend `tests/integration/lock_relay` (or add `tests/integration/lock_flow`) to drive the
  glue on `native_sim`: simulate secure-connect → unlock command → emulated pin goes active →
  disconnect → pin **stays** active → advance time past the deadline → pin goes inactive. This
  is the end-to-end proof of the two rules in the design summary, and it runs in CI.
- Host tests from B1 unchanged and still passing (the state machine did not move).

**Acceptance:** the integration suite demonstrates the full unlock/grace/relock cycle against
emulated hardware; all six targets build.

**Commit:** `feat(lock): connect the control service, lock core and relay`

---

## B5 — *(optional)* Physical unlock button

Gate on Q5 in `FEATURE_PLAN.md` §4. If wanted:

**Files:** `boards/*.overlay` (`lock-button` alias), `src/lock_button.c/.h` (pure debounce +
short/long-press classification), `tests/host/test_lock_button.c`, `src/main.c` (GPIO interrupt
→ work item → `lock_core_step`).

**Design:** unlike `bond-reset` (a boot-time polling loop, deliberately ISR-free), this is a
runtime button and needs debounce. Keep the classification pure: feed it `(level, now_ms)`
edges and let it emit `NONE`/`SHORT`/`LONG`, so the timing logic is host-testable and the ISR
stays a bare `k_work_submit`.

**Tests:** bounce bursts shorter than the debounce window produce no event; a press held past
the long threshold produces exactly one `LONG` (not a `SHORT` followed by a `LONG`); release
before the short threshold produces nothing; wraparound at `UINT32_MAX`.

**Commit:** `feat(lock): add a debounced physical unlock button`

---

## B6 — Preference persistence

**Files:** `src/lock_prefs.c/.h` (settings handler for `auto_unlock`), `src/main.c` (load after
`settings_load()`, alongside the bond restore), `tests/host/test_lock_prefs.c` (the
serialisation only — the settings backend is Zephyr).

**Design:** one settings key, `nrfproxy/auto_unlock`, a single byte. Saved when the app sends
`SET_AUTO_UNLOCK`, loaded at boot. **The lock state itself is never written** — assert this in
review; it is the one line that would quietly undo the feature.

`bond_reset_requested()` wipes the pairing; decide whether it also clears preferences.
**Recommendation: yes** — a factory reset that leaves the previous owner's auto-unlock enabled
is a surprise, and the next phone to pair inherits it.

**Tests:** encode/decode round-trip; an unset/corrupt/oversized stored value falls back to
`auto_unlock = false` (fail-safe, matching D2); factory reset clears it.

**Commit:** `feat(lock): persist the auto-unlock preference in settings`

---

## B7 — Hardware bring-up and safety checklist

Not a code commit — a checklist to run on the bench and then the bike, recorded in this file
as it is completed. Nothing in B ships to a rider before this is signed off.

**Bench, motor disconnected:**
- [ ] Boot with no phone paired: relay reads **disabled** on a meter, immediately at power-on
      and continuously through boot (this is the fail-safe claim; measure it, don't infer it).
- [ ] Pair a phone; confirm unlock energises the relay only after the link encrypts.
- [ ] Send unlock from an unencrypted link (nRF Connect, no bond) — must be rejected by the
      stack's permission check.
- [ ] Power-cycle while unlocked → boots locked.
- [ ] Pull the phone out of range while unlocked → relay stays enabled through the grace
      window, then disables.
- [ ] Bond-reset button wipes pairing and preferences; device returns to pairing mode locked.
- [ ] Measure quiescent current in both lock states — confirm the relay/MOSFET choice did not
      wreck the power budget the rest of the firmware was tuned for.

**On the bike, wheel off the ground, then a low-speed ride:**
- [ ] Locked: motor does not assist. Unlocked: normal assist.
- [ ] Confirm the display behaves sanely in the locked state (some controllers report a fault
      when the enable line is open — worth knowing before a rider sees it).
- [ ] Ride with the phone in a pocket, screen off, for 10+ minutes: no spurious relock, no
      assist interruption. **This is the test that matters most**; a false relock under load is
      the one failure mode with a safety dimension.

---

## Open items for this workstream

- Q3 (electrical spec of the motor-enable line) shapes B2's hardware note and the relay choice.
- Q5 decides whether B5 happens.
- "Known stationary" (`motion_known`) stays `false` until workstream C can parse speed out of
  the controller stream — revisit `RELOCK_HARD_MS` then, since it exists only to cover that gap.
