# Pairing / bond-lock plan (TODO.md M3)

Goal: once a phone has paired with the device, **only that phone** can reconnect. The
bond survives power loss (stored in flash), and a **hardware button** wipes it so a new
phone can take ownership. This closes TODO.md M3 (unauthenticated NUS) without needing
per-unit secrets or a display.

Status: **plan only — nothing implemented yet.** Research verified against NCS v3.3.1
(`C:\ncs\v3.3.1`) on 2026-07-02.

---

## 1. Design overview

Two modes, decided at boot by whether a bond exists in flash:

- **Pairing mode** (no bond stored): advertise openly (current behavior). The first
  central to connect and complete SMP pairing becomes the owner.
  `CONFIG_BT_MAX_PAIRED=1` makes the stack reject any further bonds.
- **Locked mode** (bond stored): advertise with the **filter accept list** containing the
  bonded peer's identity address (`BT_LE_ADV_OPT_FILTER_CONN`) — the controller ignores
  connection attempts from everyone else at the link layer. The link must also reach
  **security level 2** (encrypted with the stored LTK) before any NUS data flows.

Pairing method: **Just Works** (no display/keypad on the device → unauthenticated but
encrypted). The one-time pairing window is under the user's physical control, and after
that the accept list + LTK are the lock. Do **not** enable `CONFIG_BT_NUS_AUTHEN` — it
was checked in `nrf/subsys/bluetooth/services/nus.c` and sets
`BT_GATT_PERM_*_AUTHEN`, which requires **MITM-authenticated** pairing; Just Works can
never satisfy it and every GATT op would fail. Encryption enforcement is done at the
application layer instead (§3, step 3). Optional hardening later: a fixed passkey
(`CONFIG_BT_FIXED_PASSKEY`) on a per-unit label upgrades pairing to authenticated, at
which point `BT_NUS_AUTHEN` becomes usable.

Reset: a GPIO **held during boot** (~3 s, with LED feedback) calls
`bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)` — erases the bond from flash, clears the
accept list, boots into pairing mode. Physical access required; radio-range attackers
can't trigger it.

## 2. Bond persistence — verified per board

`CONFIG_BT_SETTINGS` stores bonds via the settings subsystem (NVS backend) in the DT
`storage_partition`. **All four boards already define one** — no overlay changes needed:

| Board | `storage_partition` | Size | Defined in |
|---|---|---|---|
| nRF52840 DK | `0xf8000` | 32 KB | `nordic/nrf52840_partition.dtsi` |
| XIAO BLE | `0xec000` | 32 KB | `nordic/nrf52840_partition_uf2_sdv7.dtsi` |
| Pro Micro (uf2) | `0xec000` | 32 KB | `nordic/nrf52840_partition_uf2_sdv6.dtsi` |
| Dongle (PCA10059) | `0xdc000` | 16 KB | board dts |

No collisions: the DK app links at `0x0` and never reaches `0xf8000`; on the UF2 boards
the code partition ends exactly at `0xec000` and `0xec000–0xf4000` is the Adafruit
bootloader's designated "user data" region (this is exactly what it's for); the dongle's
storage ends at `0xe0000`, right below the Nordic Open bootloader. Flash survives battery
removal indefinitely — the bond persists until the reset button wipes it.

## 3. Firmware implementation steps

### Step 1 — Kconfig (`prj.conf`; shared, prod build unaffected)

```
# SMP pairing + single bond
CONFIG_BT_SMP=y
CONFIG_BT_BONDABLE=y
CONFIG_BT_MAX_PAIRED=1
CONFIG_BT_FILTER_ACCEPT_LIST=y

# Bond persistence (NVS in the DT storage_partition)
CONFIG_BT_SETTINGS=y
CONFIG_SETTINGS=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
```

Cost: roughly 20–30 KB flash + a little RAM; the app partition (~790 KB) has ample room.
These stay in `prj.conf` — `prod.conf` must *not* strip them (production needs the lock
most of all).

### Step 2 — settings load + identity interplay

- Call `settings_load()` in `main()` **after `bt_enable()` and before
  `advertising_start()`** (Zephyr requirement with `BT_SETTINGS`).
- **Integration risk — `identity_init()`:** it calls `bt_id_create()` *before*
  `bt_enable()`; with `BT_SETTINGS` the stack persists identities and restores them at
  `settings_load()`. Because our address is derived deterministically from the hardware
  ID, created and restored addresses are identical, so this *should* be benign — but
  **verify on hardware**: boot twice, confirm the address is unchanged and no
  `bt/id`-related warnings appear. If the stack complains about a pre-existing identity,
  gate `bt_id_create()` on the settings-restored identity instead.

### Step 3 — security enforcement (application layer, since `BT_NUS_AUTHEN` is out)

- Add `security_changed` to the existing `bt_conn_cb`; track a `link_secure` flag
  (guard with `conn_mutex`, same discipline as `current_conn`).
- Add `security_changed` to the `bt_conn_cb` and set `link_secure` at level ≥ 2.
  **Hardware finding — do NOT send a peripheral SMP Security Request.** The original design
  called `bt_conn_set_security(conn, BT_SECURITY_L2)` in `on_connected` (later from ~1 s
  delayed work) so the peripheral would trigger pairing/encryption. On a Pixel 6a this made
  Android raise **two** pairing dialogs for a single bond — a premature pre-negotiation
  consent the instant the Security Request arrived (`pairingAlgo=0`), then the real LE Secure
  Connections consent (`pairingAlgo=3`), with the first appearing to fail (root-caused from
  the `BluetoothBondStateMachine` trace: one continuous BONDING→BONDED session, two
  `sendPairingRequestIntent`s). Delaying the request did not help. Fix: **remove it and let
  the central drive pairing** — the bafflingvision app calls `createBond()` after service
  discovery (single, Android-owned pairing); a bonded reconnect encrypts on the central's
  own initiative. Trade-off: a non-app central must initiate pairing itself.
- **Security watchdog**: schedule a `k_work_delayable` on connect; if
  `link_secure` is still false when it fires, `bt_conn_disconnect()`. Cancel it in
  `security_changed` (success) and on disconnect. **Hardware finding:** the window must
  be per-mode — ~10 s is fine in locked mode (LTK encryption is automatic), but pairing
  mode needs ~60 s to cover the user finding/accepting Android's pairing dialog.
  Disconnecting mid-SMP aborts the pairing, which Android surfaces as a "couldn't pair:
  incorrect PIN" failure (observed with a flat 10 s window).
- Gate the data paths: `nus_received` drops incoming writes and `ble_write_thread`
  skips sends while `link_secure` is false. (Without `BT_NUS_AUTHEN` the GATT perms are
  open, so this app-level gate is the actual enforcement.)
- Register `bt_conn_auth_info_cb` (`pairing_complete` / `pairing_failed`) for logging
  and to flip into locked mode immediately after the first pairing.

### Step 4 — accept-list (whitelist) advertising

- At boot after `settings_load()`: `bt_foreach_bond(BT_ID_DEFAULT, ...)` to count bonds
  and collect the peer identity address.
- If a bond exists: `bt_le_filter_accept_list_add(&peer)` and add
  `BT_LE_ADV_OPT_FILTER_CONN` to **both** advertising parameter sets (fast *and* slow —
  the fast→slow switch in `adv_slow_handler` rebuilds params). Modify
  `advertising_start()` / `adv_slow_handler` to pick open vs. filtered params from a
  `locked_mode` flag rather than duplicating the state machine.
- Deliberately **omit `BT_LE_ADV_OPT_FILTER_SCAN_REQ`**: non-owner phones can still *see*
  the device in scans (so the bafflingvision picker and nRF Connect show it, and support
  is debuggable) — they just can't connect. Revisit if invisibility is preferred.
- After `pairing_complete` in pairing mode: set `locked_mode`, add the new peer to the
  accept list (the advertiser is stopped while connected, so this is safe), and let the
  existing `recycled` → `advertising_start()` path restart advertising filtered.
- **Integration risk — rotating phone addresses (RPA):** Android advertises connection
  requests from a resolvable private address; the controller must resolve it against the
  bonded IRK for the accept list to match. Zephyr populates the controller resolving
  list from bonded keys automatically, but this is the piece most worth a dedicated
  hardware test: bond, power-cycle the phone's Bluetooth (forces a new RPA), reconnect.

### Step 5 — reset button

- New DT alias `bond-reset` per board overlay (see §4). `main.c` reads it with
  `GPIO_DT_SPEC_GET_OR` (same null-fallback pattern as the LEDs — a board without the
  alias simply has no reset button).
- Boot-time check (before `bt_enable()`): if pressed, blink the advertising LED and
  require the button held for ~3 s (simple polling loop; no interrupt machinery needed).
  Set a `factory_reset` flag.
- After `settings_load()`: if the flag is set, `bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)`
  + `bt_le_filter_accept_list_clear()`, log it, and proceed in pairing mode. (`bt_unpair`
  must run after the settings are loaded or there's nothing to delete.)
- Hold-at-boot (vs. runtime long-press) is deliberate: simplest code, no ISR/debounce
  work, and matches the physical workflow (the unit is battery-powered; the user
  power-cycles anyway). A runtime long-press can be added later if needed.

### Step 6 — LED signaling (small, optional)

Pairing mode is already visually distinct (fast blink = fast advertising); locked-mode
advertising looks the same as today. Optionally: a distinct triple-blink on successful
factory reset so the user knows the hold "took". Keep within the existing
`set_status_leds()` framework.

### Step 7 — docs

- Update `CLAUDE.md` (Conventions + Status) and `README.md` (pairing behavior, reset
  button wiring per board, "locked to first phone" note).
- Mark TODO.md M3 as addressed; while in there, fix M4 (the stale LF-clock paragraph).

## 4. Reset-button pin per board (researched against board DTS + our overlays)

| Board | Pin | Wiring | Rationale |
|---|---|---|---|
| **nRF52840 DK** | **Button 1** = `button0`/`sw0`, P0.11 | none — on-board button | Board has 4 user buttons; alias `bond-reset` to the existing `button0` node (already `GPIO_PULL_UP \| GPIO_ACTIVE_LOW`). |
| **XIAO BLE** | **D2 = P0.28** | button/jumper from the D2 pad to GND | Only free pads are D2/D3: D0/D1 (P0.02/03) = our uart1, D4/D5 = i2c1, D6/D7 = uart0, D8–D10 = spi2 (all enabled in the board dts). D2 confirmed = P0.28 in `seeed_xiao_connector.dtsi`. Alternate: D3 = P0.29. New `gpio-keys` node in our overlay, internal pull-up, active-low. |
| **Pro Micro / nice!nano** | **P0.17** | button from the P0.17 pad to GND | Sits directly below the two GND pads on the left edge — a two-pin button bridges it naturally. Taken pins: uart0 P0.09/0.10, i2c0 P1.00/P0.11, i2c1 P1.04/P1.06, spi2 P1.01/P1.02/P1.07, LED P0.15, our uart1 P0.06/P0.08. Free alternates: P0.20, P0.22, P0.24, P0.29, P0.31, P1.11, P1.13, P1.15, P0.02. New `gpio-keys` node, pull-up, active-low. |
| **Dongle (PCA10059)** | **SW1** = `button0`/`sw0`, P1.06 | none — on-board button | The dongle's one user button (the *side* button is RESET; SW1 is the top-face button). Alias to the existing node. |

All four use internal pull-ups and short to GND when pressed — no external resistor. On
the DK/Dongle nothing is soldered; on the XIAO/Pro Micro a momentary switch (or just a
tweezer-short during power-up) does the job.

## 5. Behavior edge cases (document + test)

- **Phone forgets the bond** (user removes it in Bluetooth settings) but device is
  locked: Android keeps its identity address and IRK per adapter, so the phone can still
  *connect* (accept list matches) and re-pairing overwrites the single bond slot for the
  same peer — recovers without a factory reset. Test this.
- **Phone is factory-reset / replaced**: new identity → cannot connect at all. The
  hardware reset button is the recovery path. Document prominently in the README.
- **Device is reset but the phone still holds the old bond**: the phone tries to encrypt
  with a stale LTK → encryption fails ("PIN or Key Missing") until the user forgets the
  device on the phone. The Android app should detect and explain this (see the prompt
  in §7).
- **Second phone attempts pairing in locked mode**: accept list blocks the connection
  outright — pairing never starts. In *pairing* mode with one bond already stored (can't
  happen normally, but belt-and-braces): `CONFIG_BT_MAX_PAIRED=1` makes the bond attempt
  fail → `pairing_failed` → security watchdog disconnects.

## 6. Verification plan

Build-time (all four boards, per the CLAUDE.md build wrappers):
1. `.config` has `CONFIG_BT_SMP=y`, `CONFIG_BT_SETTINGS=y`, `CONFIG_NVS=y`,
   `CONFIG_BT_MAX_PAIRED=1`; flash offsets unchanged (DK `0x0`, XIAO `0x27000`,
   Pro Micro `0x26000`, Dongle `0x1000`); still no `partitions.yml`.
2. Image size still fits with the ~25 KB growth.

Hardware (user; suggest the DK or Dongle first for console visibility):
1. Fresh flash → pairing mode; phone A pairs (Just Works dialog) → data flows.
2. Phone B: sees the device in a scan, **cannot connect** while locked.
3. Power-cycle the device → still locked to phone A; phone A reconnects + encrypts
   without re-pairing. **Also toggle phone A's Bluetooth off/on (new RPA) and confirm
   reconnect** — this validates IRK resolution against the accept list.
4. Boot twice, confirm the BT address is stable (the `identity_init` ↔ settings
   interplay from §3 step 2).
5. Hold reset button through boot ~3 s → bond wiped → phone B can now pair.
6. Regression: fast→slow advertising switch, disconnect→reconnect cycle, and the
   `recycled` paths still behave (no `-EALREADY`, no dead advertiser).

Unit-test hooks (fold into TODO.md's test plan): the mode decision (bonds→locked),
accept-list population, watchdog disconnect logic, and reset-button hold detection are
all state-machine logic that fits the planned `proxy_core.c` extraction.

## 7. Prompt for the Android side (bafflingvision)

Paste the following into a Claude Code session in the `bafflingvision` repo once the
firmware side is implemented:

---

The nrfProxy firmware (the BLE↔UART bridge this app talks to over NUS) is gaining
**bond-to-first-phone security**. Firmware behavior after the change:

- On first connection the **app initiates pairing** with `createBond()` after service
  discovery (the firmware does *not* send a Security Request); Android shows the system
  **Just Works pairing dialog** (no PIN). The resulting bond is stored on both sides.
- After pairing, the device only accepts connections from the bonded phone (link-layer
  filter accept list). Other phones still *see* it in scans but connect attempts time
  out.
- NUS data is dropped by the firmware until the link is **encrypted** (security level 2).
  The firmware disconnects any link that never encrypts (~10 s once bonded/locked; ~60 s
  during first-time pairing so the user has time to accept the dialog).
- A hardware button on the device wipes the bond (factory reset to "pairing mode").

Update `transport/BleSerialLink.kt` (and related UI) to handle this. Requirements:

1. **Bonding flow**: after `connectGatt` + service discovery, the firmware's Security
   Request will trigger Android's pairing dialog automatically. Register a
   `BroadcastReceiver` for `BluetoothDevice.ACTION_BOND_STATE_CHANGED` for the target
   device during connection; if state goes `BOND_BONDING`, hold off on GATT operations
   (especially the CCC descriptor write that enables NUS notifications) and retry them
   once `BOND_BONDED` arrives. If the CCC write or a characteristic write fails with
   GATT_INSUFFICIENT_AUTHENTICATION (5) / GATT_INSUFFICIENT_ENCRYPTION (15), treat it
   the same way: wait for bonding, then retry once.
2. **UI states**: surface "Pairing…" during `BOND_BONDING` (map into the existing
   transport-neutral `UiState` machinery — probably as a `Connecting` message variant,
   not a new state, unless a new state is cleaner). On `BOND_NONE` after a bonding
   attempt, show a terminal `ConnectionError` ("Pairing was rejected or failed").
3. **Locked-out detection**: if the device is bonded to a *different* phone, our
   `connectGatt` will just time out even though the scan saw the device. After the scan
   succeeded but connection times out repeatedly (2–3 attempts), show a helpful error:
   "Device may be paired to another phone. Hold the reset button on the device while
   powering it on to unpair it." Don't loop the auto-reconnect forever in this case —
   this is a config-level `ConnectionError` with a Retry button, not `Reconnecting`.
4. **Stale-bond detection (device was factory-reset)**: if the phone *has* a bond
   (`device.bondState == BOND_BONDED`) but connections drop immediately or encryption
   fails (fast disconnect after connect, status 133 loops), the firmware bond was likely
   wiped. Show: "Pairing is out of date — forget this device in Android Bluetooth
   settings, then reconnect." (Apps cannot programmatically remove a bond without
   reflection; just instruct the user.) Detect via: bonded device + N consecutive
   immediate disconnects.
5. **Keep the existing architecture**: scan-then-connect stays (it's load-bearing for
   unbonded random-address devices per CLAUDE.md — and still needed for the *first*
   connect before any bond exists). Auto-reconnect (`UiState.Reconnecting`, 3 s retry)
   stays for the bonded happy path. `BLUETOOTH_CONNECT` permission is already held and
   covers bonding. NUS UUIDs, MTU negotiation, and the write path are unchanged.
6. **Device picker**: unchanged (the device stays scannable). Optionally show a lock
   hint if `bondState == BOND_BONDED` for a listed device.

Test plan: first-pair happy path (dialog → bonded → data flows); reconnect after app
restart and after phone BT toggle (no new dialog); second-phone lockout message;
firmware-reset stale-bond message; pairing-dialog cancel → clean error, retry works.

---

*End of Android prompt.*
