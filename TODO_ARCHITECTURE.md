# TODO_ARCHITECTURE.md — review follow-ups (2026-07-11)

Findings from the whole-project architecture/code review of 2026-07-11, written up as
implementable tasks. Read `CLAUDE.md` first — it has the build environment, the
per-board gotchas, and the concurrency conventions this file assumes. Related history
lives in `TODO.md` (2026-07-02 review; M2/L1/L3 there are picked up here as tasks 3, 4,
and 7).

Ground rules for whoever implements this (per CLAUDE.md conventions):

- **Do not run git write commands** — the user handles all commits/branches. Suggest a
  commit at good stopping points.
- Every change to `src/main.c` or a `.conf`/`.overlay` must be **compile-verified on all
  six configurations** via the wrapper: `.\build.ps1` (targets: `dk`, `xiao`,
  `xiao_prod`, `promicro`, `promicro_prod`, `dongle`; no arg = all). After building,
  verify per CLAUDE.md: no `partitions.yml` in any build dir; flash offsets unchanged
  (DK `0x0`, XIAO `0x27000`, Pro Micro `0x26000`, Dongle `0x1000`);
  `CONFIG_UART_1_ASYNC=y` in `build*/nrfProxy/zephyr/.config`.
- Code style: Zephyr/Linux kernel — tabs, `LOG_*`, `K_*_DEFINE` static objects. Match
  the existing comment density in `main.c` (comments explain *constraints*, not what
  the next line does).
- Hardware testing is done by the user, not in-session. Where a task needs it, list the
  exact test in your summary and mark the task "compile-verified, awaiting hardware".
- Concurrency discipline (do not violate): `current_conn` / `link_secure` /
  `adv_active` are guarded by `conn_mutex`; anything touching `current_conn` takes its
  own `bt_conn_ref()` under the mutex before use; the ring buffers are strictly
  single-producer/single-consumer (see the `uart_rx_ringbuf_drain()` comment block for
  why `ring_buf_reset()` is forbidden).

Tasks are in recommended order. 1 and 2 are quick; 3+4 are one small feature; 5 is a
test scaffold; 6 is a refactor to do *before* hook logic lands; 7–8 are cleanup.

## Progress (2026-07-23)

- ✅ **Task 1** — done (docs only). `PAIRING_PLAN.md` §7 req 1 rewritten for the
  `createBond()` flow + Android 14 `RECEIVER_EXPORTED` note; status line corrected.
- ✅ **Task 2** — done. Collapsed `SECURITY_TIMEOUT_LOCKED`/`_PAIRING` into a single
  `SECURITY_TIMEOUT` (60 s) via `src/security_timeout.h` (`SECURITY_TIMEOUT_MS`);
  docs updated (CLAUDE/README/PAIRING_PLAN §5+§6/ARCHITECTURE); host test under
  `tests/host/`. NCS not on this machine — firmware compile-verify deferred.
- ✅ **Task 7 item 5** — done (docs only). README intro reordered; debug-vs-production
  wording disambiguated. The rest of Task 7 (items 1–4) edits `src/main.c` and is still
  pending.
- ⛔ **Tasks 3, 4, 5, 6, and Task 7 items 1–4** — still outstanding (separate branches).
- **Task 8** — reference checklist; no code action by design (deferred).

---

## Task 1 — Fix the stale/contradictory parts of PAIRING_PLAN.md  *(docs only, 5 min)*

`PAIRING_PLAN.md §7` is a prompt meant to be pasted into the `bafflingvision` Android
repo. Its intro paragraph was updated for the central-driven pairing flow (the app
calls `createBond()` after service discovery; the firmware deliberately sends **no**
SMP Security Request — see the long comment above `ref_unsecured_conn()` in
`src/main.c` and the "Just Works" bullet in CLAUDE.md). But **requirement 1 inside the
same prompt still describes the OLD flow**: "the firmware's Security Request will
trigger Android's pairing dialog automatically". Anyone following that prompt builds
the app without the `createBond()` call the whole flow depends on.

Changes:

1. Rewrite §7 requirement 1 so the bonding flow is: after `connectGatt` + service
   discovery, the app **calls `createBond()`** (Android then shows the single Just
   Works dialog). Keep the rest of the requirement (register a `BroadcastReceiver` for
   `ACTION_BOND_STATE_CHANGED`; hold off GATT ops — especially the CCC write — during
   `BOND_BONDING` and retry on `BOND_BONDED`; treat GATT errors 5/15 the same way).
   Note the receiver must be registered `RECEIVER_EXPORTED` on Android 14+ (a real bug
   already hit and fixed — see CLAUDE.md "Status" section).
2. Fix the status line near the top of the file: it still says "plan only — nothing
   implemented yet". It should say the plan is implemented and hardware-tested except
   the items listed in §6 (address stability, RPA-vs-accept-list) and note the
   createBond change of §3 step 3 is compile-verified but not yet hardware-confirmed.
3. While in §5: the first edge case ("phone forgets the bond … recovers without a
   factory reset") is exactly what Task 2 fixes — after doing Task 2, update that
   bullet to mention the watchdog window must (and now does) allow a human dialog in
   locked mode, and add the §6 hardware test from Task 2.

No firmware changes; no build needed.

## Task 2 — Locked-mode watchdog breaks bond-loss recovery  *(one-constant fix + comments)*

**Bug.** The security watchdog window is per-mode: `SECURITY_TIMEOUT_LOCKED` = 10 s,
`SECURITY_TIMEOUT_PAIRING` = 60 s (`src/main.c`, `#define`s above
`security_timeout_work`; armed in `on_connected`). The 60 s pairing window exists
because disconnecting mid-SMP aborts pairing and Android shows "couldn't pair:
incorrect PIN" — observed on hardware; the human needs time to find/accept the dialog.

But there is a **locked-mode path that also needs a human dialog**: the owner phone
"forgets" the device in Android Bluetooth settings. Android keeps its identity address
and IRK, so the phone still passes the filter accept list and can connect — and
re-pairing overwrites the single bond slot (`CONFIG_BT_MAX_PAIRED=1`, same peer). This
is the documented no-factory-reset recovery path (`PAIRING_PLAN.md §5`, first bullet).
Re-pairing shows a dialog; the 10 s locked window kills it mid-SMP — the exact failure
mode already root-caused during pairing-mode testing. Result: the recovery path almost
certainly fails on hardware, and the user is forced to the physical reset button.

**Why the fix is safe.** In locked mode the *filter accept list* is the real gate: with
`BT_LE_ADV_OPT_FILTER_CONN`, the controller ignores connection attempts from any peer
not on the list at the link layer. A stranger can't connect at all, so the watchdog is
not defending the single connection slot against attackers — it only bounds a
misbehaving *authorized* peer (or an attacker holding the owner's IRK, who has already
won). Lengthening it has no security cost.

**Change** (in `src/main.c`):

1. Set `SECURITY_TIMEOUT_LOCKED` to `K_SECONDS(60)` — or simpler, collapse the two
   defines into one `SECURITY_TIMEOUT` of 60 s and drop the per-mode selection in
   `on_connected` (`locked_mode ? … : …`). Prefer the collapse: less state, and the
   per-mode distinction no longer buys anything.
2. Rewrite the comment block above the defines: the current text argues the 10 s locked
   window "keeps squatters from blocking the owner" — that argument is wrong-in-practice
   because squatters can't get past the accept list. The new comment must give (a) the
   dialog-abort/"incorrect PIN" reason the window is long, (b) the bond-loss-recovery
   scenario that requires it to be long *in locked mode too*, and (c) why long is safe
   (accept list is the gate).
3. Update the matching prose in `CLAUDE.md` (the "⚠️ The watchdog window is per-mode"
   bullet under "Pairing lock" and the mention in "Status") and `README.md` ("after
   ~10 s once locked" in the Pairing section).

**Verify:** all six builds compile; nothing else changes. Ask the user to add this
hardware test: bond phone A → in Android Bluetooth settings "forget" the device →
reconnect from the app → expect ONE pairing dialog, take >10 s to accept it → pairing
succeeds and data flows. Also retest the normal bonded reconnect (should encrypt within
a couple of seconds; the watchdog never fires).

## Task 3 — UART RX can die permanently; add retry  *(TODO.md M2, promoted)*

**Bug.** The `UART_RX_DISABLED` case in `uart_cb()` (`src/main.c`) is the *only*
recovery path when async reception stops (e.g. the slab was empty at
`UART_RX_BUF_REQUEST` time). Inside it, if `k_mem_slab_alloc()` (K_NO_WAIT, ISR) or
`uart_rx_enable()` fails, reception is never re-enabled: the serial→BLE direction —
the device's primary function — silently dies while LEDs/BLE look healthy. The
production build has **no logs**, so the field symptom is "bridge half-dead until
power cycle" with zero diagnostics.

**Fix direction** (pattern already exists in this file — mirror `adv_retry_work`):

1. Add a `k_work_delayable uart_rx_retry_work` (init in `main()` next to the others).
   Handler: try `k_mem_slab_alloc` + `uart_rx_enable` (this runs in the system
   workqueue, thread context — K_NO_WAIT alloc still, don't block the workqueue); on
   any failure, free the buffer if allocated, log `LOG_WRN` **once per outage** (a
   `static bool` latch reset on success is fine), and `k_work_reschedule` itself
   (~100 ms is fine; it's an exceptional path).
2. In the `UART_RX_DISABLED` ISR case: on either failure, `k_work_reschedule` the retry
   work instead of giving up. Keep the existing inline first attempt — the work item is
   the fallback, not the primary path.
3. Do NOT touch the `UART_RX_BUF_REQUEST` case — its deliberate answer-with-nothing
   behavior funnels into `UART_RX_DISABLED`, which is the recovery point (the comment
   there says exactly this; keep that invariant).
4. Mind the race: the retry handler and a concurrent `UART_RX_DISABLED` ISR could both
   try to re-enable. `uart_rx_enable()` on an already-enabled port returns `-EBUSY` —
   treat `-EBUSY` as success (someone else won) and don't log it as a failure.
5. Update `TODO.md` M2 to ✅ with a one-line note, same style as the other closed items.

**Verify:** six builds compile. Hardware check for the user (bench, DK or Dongle for
console): stream serial data, confirm normal operation; there's no easy way to force
the failure without instrumenting, so the main assurance is review + the -EBUSY
handling.

## Task 4 — Drop counters for the silent data-loss points  *(TODO.md L1)*

Three places lose data with no trace (`src/main.c`):

- `UART_RX_RDY` in `uart_cb()`: `ring_buf_put()` return value ignored — bytes beyond
  the free space vanish (2048-byte ring ≈ 180 ms of 115200; a slow/backgrounded phone
  can overflow it while connected).
- `uart_tx_kick()`: a failing `uart_tx()` drops the staged chunk (deliberate — keep the
  drop, but count it and log the error code).
- `UART_TX_ABORTED` in `uart_cb()`: the aborted remainder is treated as sent.

**Fix direction.** ISR-safe counters + periodic thread-context reporting:

1. Add `static atomic_t` counters (e.g. `drops_uart_rx_ring`, `drops_uart_tx`) —
   `atomic_add` from ISR context is fine; do NOT log from the ISR.
2. Report from `ble_write_thread`: after its drain loop (or on a coarse time check,
   e.g. at most once per 10 s), if any counter changed since last report, one
   `LOG_WRN("drops: uart_rx_ring=%u uart_tx=%u", ...)` with cumulative totals.
   Cumulative totals, not deltas — they double as a crude health metric.
3. `uart_tx_kick()` failure: also latch the last `uart_tx()` error code into an
   `atomic_t` so the periodic report can include it (`LOG_WRN` directly is not safe —
   `uart_tx_kick` can run in ISR context via `UART_TX_DONE`; logging *is* allowed in
   ISRs with deferred mode, but keep it out anyway for consistency).
4. Update `TODO.md` L1 to ✅.

Keep it small — this is observability, not flow control. Do not add backpressure.

**Verify:** six builds compile; in a debug build, saturating the phone side (or just
unplugging the phone mid-stream, letting the ring wrap while unconnected — note
disconnect-drain is intentional and should NOT count as a drop; only the *put*-side
overflow counts) produces the periodic warning.

## Task 5 — Twister build-only regression matrix  *(TODO.md test-plan item 10 — do this before the refactor)*

This project's bug history is dominated by config regressions (Partition-Manager 0x0
link, uart1 async `-ENOSYS`, prod.conf stripping). Mechanize the checks that are
currently manual:

1. Create `tests/` (or use app-level `sample.yaml`/`testcase.yaml` — investigate which
   NCS v3.3.1 Twister supports for an *application* repo; a `testcase.yaml` at repo
   root driving build-only configurations is the usual shape).
2. Build-only entries for all six configurations (four boards + the two `prod`
   variants via `EXTRA_CONF_FILE=prod.conf`), each passing
   `-DSB_CONFIG_PARTITION_MANAGER=n` (it's a sysbuild Kconfig — check how Twister
   passes sysbuild args in this NCS version; `extra_args:
   SB_CONFIG_PARTITION_MANAGER=n` is the likely form).
3. Post-build assertions per configuration — Twister's `filter`/`required_config` can
   check Kconfig, or add a tiny script step:
   - `CONFIG_FLASH_LOAD_OFFSET`: DK `0x0`, XIAO `0x27000`, Pro Micro `0x26000`,
     Dongle `0x1000`;
   - `CONFIG_UART_1_ASYNC=y` everywhere;
   - prod variants: `CONFIG_LOG` unset, `CONFIG_SERIAL=y`, and `CONFIG_BT_SMP=y` /
     `CONFIG_BT_SETTINGS=y` / `CONFIG_NVS=y` still present (the "prod.conf must not
     strip the pairing lock" invariant);
   - no `partitions.yml` anywhere in the build dir.
4. Document how to run it (a short section in CLAUDE.md + README): Twister lives in
   the NCS tree; it must run with the CLAUDE.md environment block, from `C:\ncs\v3.3.1`.
   If Twister turns out not to support this app/sysbuild combination cleanly in
   v3.3.1, fall back to a `check-builds.ps1` script that loops the six wrapper builds
   and greps `.config` — the assertions matter more than the harness.

## Task 6 — Extract `proxy_core.c` and move the serial-side hook out of ISR context  *(do BEFORE writing real hook logic)*

Two coupled structural items (TODO.md test-plan preamble + I2):

**6a. Move `on_uart_rx` processing out of the ISR.** Today `uart_cb()`'s `UART_RX_RDY`
case runs the hook in ISR context and puts the *processed* output in the ring. The
hooks are "the user's main extension point" — framing/checksum/filter logic must not
live in an ISR. Restructure the serial→phone path so the ISR does only
`ring_buf_put(raw bytes)` + `k_sem_give`, and `ble_write_thread` runs `on_uart_rx`
*after* claiming from the ring, before `bt_nus_send`.

Consequences to handle (think these through; they're the actual work):
- The hook currently sees UART-delivered chunk boundaries; after the move,
  `ble_write_thread` claims up to `max_send` contiguous bytes, so chunking seen by the
  hook changes. That's fine — CLAUDE.md already documents hooks as chunk-based with no
  framing guarantees — but say so in the hook's comment.
- The hook can grow data (up to `PROC_BUF_SIZE`=512) while `max_send` is MTU-3
  (≤244): claim at most `max_send` *post-hook* bytes worth. Simplest correct scheme:
  claim ≤ `max_send`, run hook into `proc_buf`, and if `out_len > max_send` send in
  slices from `proc_buf` (loop `bt_nus_send` over it, honoring the existing
  `-ENOMEM`/`-EAGAIN` keep-and-retry semantics — note that once the hook has consumed
  ring bytes you can no longer "keep data in the ring", so the retry loop must retry
  from `proc_buf`, not re-claim).
- `ring_buf_get_finish()` must still reflect *ring* bytes consumed (pre-hook length),
  which now differs from bytes sent.

**6b. Extract testable core.** Create `src/proxy_core.c` + `src/proxy_core.h` with the
pure logic, leaving `main.c` as glue/callbacks: the two hooks; `identity_init()`'s
derivation (hwid → address bytes/name suffix/mfg-data, taking the hwid as a parameter
so it's testable without hwinfo); and the advertising/connection state-machine
*decisions* if separable without contortion (the `adv_active`/`current_conn` guard
predicates — don't force it; the BT-API calls stay in `main.c`). Add the new file to
`CMakeLists.txt` (`target_sources`). Keep behavior identical — this task plus 6a should
produce zero functional change on the wire except hook execution context.

Then add the first ztest suites against `proxy_core` per TODO.md items 1–3
(hooks: clamp/drop/grow/zero-len; identity: address MSBs forced static, suffix
format, mfg layout, <6-byte-hwid fallback). `native_sim` target via Twister.

**Verify:** six builds + Twister matrix from Task 5 still green; hook behavior
unchanged for pass-through (bridge works identically). Ask the user to bench-test
serial→phone streaming after 6a (chunking changed; watch for regressions at sustained
115200).

## Task 7 — Small code hygiene  *(batch, low risk)*

All in `src/main.c` unless noted:

1. **`bt_set_name()` ordering note.** `main()` calls `bt_set_name(device_name)` before
   `settings_load()`; with `CONFIG_BT_DEVICE_NAME_DYNAMIC` + `CONFIG_BT_SETTINGS` the
   stored name is restored at `settings_load()` and overwrites the runtime one. Benign
   today *only because* the name is deterministically re-derived each boot (stored ==
   set). Either move `bt_set_name` after `settings_load()` (preferred; also update the
   comment) or add a comment stating the determinism assumption. If moved: confirm
   `ad[1].data_len = strlen(device_name)` still happens before `advertising_start()`.
2. **`locked_mode` mutex exemption comment.** `locked_mode` is read without
   `conn_mutex` in `on_connected` and `adv_params_fast/slow()` while everything
   adjacent follows the mutex discipline. It's safe (single-writer bool; writes happen
   at boot pre-connection or from `pairing_complete` while connected) — add one comment
   at its declaration saying exactly that, so the exemption is visibly deliberate.
3. **`uart_tx_ringbuf` disconnect asymmetry comment.** `on_disconnected` flushes the
   RX ring (via the writer thread) but deliberately not the TX ring (≤2048 B drains in
   ~180 ms at 115200, and resetting from the BT thread would violate SPSC). Add a
   sentence to the existing disconnect comment so it reads as a decision, not an
   omission.
4. **Blink-phase reset (TODO.md L3, cosmetic part).** `adv_blink_handler`'s
   `static bool on` carries across advertising sessions, so re-entering advertising can
   start with an off-gap. Cheapest fix: make `on` a file-scope static and clear it in
   `set_status_leds()` when entering `STATUS_ADVERTISING`. Leave the broader LED-race
   item (L3 first half) alone — it's benign and a rework isn't warranted.
5. **README intro flow.** Lines ~6–8 interrupt "acting as a **bidirectional proxy**:"
   before its bullet list with two unrelated paragraphs (the Claude note and the
   production-build note). Move both below the bullets. Also address TODO.md L4's
   remaining nit: disambiguate "debug build of the production board" wording for the
   XIAO row/fragment table.

## Task 8 — Pre-ship checklist  *(no action now; keep visible)*

- Company ID in `mfg_data` is `0xFFFF` (SIG test ID) — must be replaced with an
  assigned ID before shipping (also update the matching Android ScanFilter).
- Hardware verifications still owed (from CLAUDE.md/PAIRING_PLAN §6): single pairing
  dialog with the createBond flow; BT address stable across two boots
  (identity↔settings interplay); owner reconnect after phone BT toggle (RPA resolution
  against the accept list); Task 2's forget-and-re-pair test.
- MTU: the firmware never initiates ATT MTU exchange (peripheral); a central stuck at
  23-byte MTU gets 20-byte notifications and a sustained 115200 stream can outrun the
  link. Known/accepted (TODO.md I1) — revisit only if a non-app central matters.
