# ADD_TESTING_PLAN.md — bring nrfProxy under test + CI/CD + Dependabot

Written 2026-07-16 for hand-off to an implementing agent (Sonnet). Read `ARCHITECTURE.md`
first (system description + testability analysis), then `CLAUDE.md` (build environment,
per-board gotchas, concurrency rules). This plan supersedes `TODO.md`'s test-plan section
and `TODO_ARCHITECTURE.md` Task 5; it incorporates Task 6 as Phases 2 and 4.

Decisions already made by the project owner (do not re-litigate):

- **CI**: GitHub Actions, hosted Linux runners, Nordic's NCS toolchain **Docker
  container** pinned to v3.3.1. No self-hosted runners.
- **Refactor**: the `proxy_core` extraction (and the `uart_bridge` extraction it
  enables) **is in scope** as a prerequisite for unit tests.
- **BabbleSim**: in scope as the **final, stretch phase** — CI must be green without it.
- **Dependabot**: `github-actions` ecosystem only.

## Ground rules (from CLAUDE.md — binding)

- **Do not run git write commands.** The user handles all commits/branches/pushes.
  Suggest a commit at the end of each phase.
- Every change to `src/*.c/h`, `CMakeLists.txt`, or any `.conf`/`.overlay` must be
  **compile-verified on all six configurations** (`.\build.ps1` locally; the CI matrix
  once Phase 1 lands). After building verify: no `partitions.yml` in any build dir;
  offsets DK `0x0` / XIAO `0x27000` / Pro Micro `0x26000` / Dongle `0x1000`;
  `CONFIG_UART_1_ASYNC=y` — all in `build*/nrfProxy/zephyr/.config`.
- Code style: Zephyr/Linux kernel — tabs, `LOG_*`, `K_*_DEFINE`. Comments explain
  constraints, not the next line.
- Concurrency discipline (never violate): `current_conn`/`link_secure`/`adv_active`
  under `conn_mutex`; own `bt_conn_ref()` under the mutex before use; ring buffers are
  strictly SPSC; `ring_buf_reset()` forbidden.
- Hardware testing is the user's job. Where a phase changes runtime behavior, list the
  exact bench test in your summary and mark it "compile-verified, awaiting hardware".

## Environment facts the implementer needs

- SDK: nRF Connect SDK **v3.3.1** (Zephyr 4.3.99). Local install `C:\ncs\v3.3.1`,
  toolchain `C:\ncs\toolchains\936afb6332` (env block in CLAUDE.md). Local host is
  **Windows**; `native_sim` tests therefore run in CI (or WSL), not natively on the
  dev machine — document this, don't fight it.
- This is a **standalone application repo** (no west.yml of its own; the workspace is
  the external NCS install). In CI you must materialize an NCS v3.3.1 workspace and
  build this repo as an out-of-tree app, mirroring the local flow
  (`west build` from the workspace with the app dir as source).
- All six configs pass the sysbuild Kconfig `-DSB_CONFIG_PARTITION_MANAGER=n`; prod
  variants add `-DEXTRA_CONF_FILE=prod.conf`.

### ⚠ Verify-before-use list (assumptions that may differ in NCS v3.3.1 — check, don't assume)

1. **Toolchain container image + tag.** Expected:
   `ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.3.1` (Nordic's published toolchain image).
   Verify the exact name/tag exists (check the sdk-nrf repo's CI workflows for what
   Nordic itself uses). Fallback: any Ubuntu image + `nrfutil sdk-manager` /
   Zephyr-SDK manual install; last resort `nordicplayground/nrfconnect-sdk` images.
2. **Twister + sysbuild**: that scenario-level `sysbuild: true` is supported and that
   `extra_args: [SB_CONFIG_PARTITION_MANAGER=n]` reaches sysbuild (both are standard
   in Zephyr ≥ 3.7, so they should be — but confirm with one scenario before writing
   all six). Also confirm `west twister` is available (else invoke
   `zephyr/scripts/twister` directly).
3. **`uart_emul` async-API support** on `native_sim` (needed for E-group tests). If
   the emulated driver lacks the async API in this tree, fall back to testing
   `uart_bridge` against a **stub uart layer** (function pointers, Phase 4 option B).
4. **BabbleSim board name** (`nrf52_bsim` vs `nrf52_bsim/native`) and whether the NCS
   workspace already carries the `babblesim` modules (`west list | grep bsim`).
5. **`bt_nus_client`** availability in NCS v3.3.1 for the bsim central (it's a
   long-standing NCS library; should exist).

If a verification fails, implement the documented fallback and record the finding in
this file (edit it — it's a living plan).

### Findings log (filled in as the phases land)

- **Item 1 — toolchain container: CONFIRMED.** `ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.3.1`
  exists (tagged alongside `v3.3.1-rc1`, `v3.3.0`, …). It carries the **toolchain only**,
  not the SDK source, so CI still has to `west init` a workspace — as the plan assumed. Its
  Dockerfile sets `ENV BASH_ENV=/opt/non-interactive-setup.sh`, which sources
  `/opt/toolchain-env.sh`, so the toolchain env (PATH, `ZEPHYR_SDK_INSTALL_DIR`) loads
  automatically in GitHub Actions' non-interactive bash — no `nrfutil toolchain-manager
  launch` wrapper needed. The `setup-ncs` composite action asserts this instead of assuming
  it (`west --version` + a check that the `nrf` module is present).
- **Phase 0 deviation — no `if: false` placeholder jobs.** The job graph is wired as a
  comment block at the top of `ci.yml` and each phase appends its real job. Disabled
  placeholder jobs would have been dead weight in the final tree; the comment serves the
  same purpose (later phases only add a job) and keeps every commit's CI meaningful.
- **⚠ The development machine cannot build this project.** Contrary to CLAUDE.md, this
  checkout's host has **no NCS install** (`C:\ncs` and `D:\ncs` are both absent/empty), **no
  Python**, and **no WSL distribution** — only mingw64 `gcc` 13.2 and git. Consequences,
  which shaped the phases below: `check_configs.py` and the six-config matrix are
  **CI-verified only**; and `proxy_core` was deliberately designed **free of every Zephyr
  dependency** (see Phase 2) so its logic can be compiled and exercised on the host with
  plain gcc as a local smoke check, independent of `native_sim`. Anything marked
  "CI-verified" below has not run on this machine.

---

## Phase 0 — Repo scaffolding, Dependabot, CI skeleton  *(no firmware changes)*

Deliverables:

1. **`.github/dependabot.yml`**:

   ```yaml
   version: 2
   updates:
     - package-ecosystem: "github-actions"
       directory: "/"
       schedule:
         interval: "weekly"
       groups:
         actions:
           patterns: ["*"]
   ```

   (Grouped so action bumps arrive as one PR. No other ecosystems apply — there are no
   package manifests in this repo; the NCS dependency is pinned by CI workflow env,
   not by a manifest Dependabot understands.)

2. **`.github/workflows/ci.yml` skeleton** with the job graph below (jobs land across
   Phases 1/3/4; wire the skeleton now so every later phase only adds a job):

   ```yaml
   name: CI
   on:
     push: { branches: [main] }
     pull_request:
   concurrency:
     group: ci-${{ github.ref }}
     cancel-in-progress: true
   env:
     NCS_REV: v3.3.1
   jobs:
     build-matrix:   # Phase 1 — six firmware configs + config assertions
     unit-tests:     # Phase 3 — twister, native_sim ztest suites
     integration:    # Phase 4 — twister, native_sim uart_bridge tests (may merge into unit-tests)
     lint:           # Phase 0 — shellcheck build.sh; (optional) PSScriptAnalyzer build.ps1
     # bsim:         # Phase 5 — nightly/manual, separate workflow, never blocks CI
   ```

   Shared plumbing for the west-workspace jobs (write once as a composite action under
   `.github/actions/setup-ncs/`):
   - runs in the toolchain container (`container: image: <verified image>:v3.3.1`);
   - `west init -m https://github.com/nrfconnect/sdk-nrf --mr $NCS_REV ncs && cd ncs &&
     west update --narrow -o=--depth=1` (shallow — the full workspace is many GB);
   - cache the workspace with `actions/cache` keyed on `NCS_REV` (restore-keys off the
     same prefix). If the workspace exceeds cache limits, cache only `ncs/modules` +
     `ncs/zephyr` + `ncs/nrf`, or accept the fetch cost — measure first, don't
     prematurely optimize;
   - checkout this repo *outside* the workspace (e.g. `$GITHUB_WORKSPACE/app`) and
     build with `west build ... $GITHUB_WORKSPACE/app`, matching the local flow.

3. **README badge + a "Testing" section** in README.md: how to run the matrix locally
   (`.\build.ps1` + `scripts/check_configs.py`), that unit tests run on
   `native_sim` in CI/WSL, and a pointer to this plan.

Acceptance: dependabot.yml valid (GitHub UI shows the update config); `lint` job green;
workflow syntax-valid even while the other jobs are `if: false` placeholders.

## Phase 1 — Build/config regression matrix  *(highest value per effort — do before any refactor)*

Mechanizes the manual `.config` checks. Two layers:

1. **Twister build-only scenarios.** Add `tests/build/testcase.yaml` (a build-only
   Twister test app is unnecessary — point Twister at the real app):
   the cleanest shape for an out-of-tree app is a repo-root `testcase.yaml` (or
   `sample.yaml`) with one scenario per configuration:

   ```yaml
   tests:
     app.build.dk:
       sysbuild: true
       build_only: true
       platform_allow: [nrf52840dk/nrf52840]
       extra_args: [SB_CONFIG_PARTITION_MANAGER=n]
     app.build.xiao:
       # xiao_ble/nrf52840, same extra_args
     app.build.xiao_prod:
       # + EXTRA_CONF_FILE=prod.conf   (twister: extra_args: [EXTRA_CONF_FILE=prod.conf] — verify quoting)
     app.build.promicro:
       # promicro_nrf52840/nrf52840/uf2 — NOTE: community board from the `others/` tree;
       # if twister can't resolve it, pass --board-root appropriately or fall back to (2).
     app.build.promicro_prod:
     app.build.dongle:
   ```

   Run: `west twister -T <this repo> -v --integration` (from the workspace, toolchain
   env active). **If Twister fights the sysbuild/out-of-tree/community-board
   combination for more than ~a day of effort, drop it** and let layer (2) drive plain
   `west build` loops — the assertions matter, not the harness.

2. **`scripts/check_configs.py`** (Python 3, stdlib only — runs on Windows + CI):
   given a build dir, assert:
   - `CONFIG_FLASH_LOAD_OFFSET` per board: DK `0x0`, XIAO `0x27000`, Pro Micro
     `0x26000`, Dongle `0x1000`;
   - `CONFIG_UART_1_ASYNC=y` and `CONFIG_UART_1_INTERRUPT_DRIVEN` not `y` — all builds;
   - `CONFIG_BT_MAX_CONN=1`, `CONFIG_BT_MAX_PAIRED=1`, `CONFIG_BT_FILTER_ACCEPT_LIST=y`;
   - prod variants: `CONFIG_LOG` **not** set, `CONFIG_SERIAL=y`, and the
     pairing-lock set intact: `CONFIG_BT_SMP=y`, `CONFIG_BT_SETTINGS=y`,
     `CONFIG_NVS=y` (the "prod.conf must not strip the lock" invariant);
   - **no `partitions.yml` anywhere** under the build dir (PM really off);
   - exit non-zero with a per-check report.
   Table of expected values lives in the script keyed by target name (`dk`, `xiao`,
   `xiao_prod`, `promicro`, `promicro_prod`, `dongle`) so it works after both
   `build.ps1` and CI builds.

3. **CI `build-matrix` job**: strategy matrix over the six targets → `west build`
   (same args as the wrapper scripts) → `check_configs.py` → upload
   `zephyr.hex`/`zephyr.uf2` as artifacts (they're the flashable outputs; retention
   ~14 days).

Test IDs (referenced later): **A1–A6** = six configs build; **A7** = offset assertions;
**A8** = async-UART assertions; **A9** = prod-strip assertions; **A10** = no
partitions.yml; **A11** = pairing Kconfig assertions.

Dependencies: Phase 0 skeleton. No firmware changes.
Acceptance: CI red if any assertion is deliberately broken (spot-check by locally
flipping one value in a scratch build), green on the real tree.

## Phase 2 — Extract `proxy_core` (pure logic)  *(prerequisite for Phase 3)*

Create `src/proxy_core.c` + `src/proxy_core.h`; add to `CMakeLists.txt`
`target_sources`. `main.c` keeps all Zephyr-API calls and callbacks; `proxy_core`
holds **pure, context-free logic** — no BT host calls, no logging, no statics that
hold kernel objects. Behavior must be bit-identical.

Suggested API (adjust names to taste, keep the shape):

```c
/* Interception hooks (moved from main.c, now non-static, unchanged semantics). */
size_t on_uart_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);
size_t on_ble_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);

/* Identity derivation — parameterized on the hwid so tests need no hwinfo driver. */
struct proxy_identity {
	bt_addr_le_t addr;        /* valid only if addr_valid */
	bool addr_valid;
	char name[CONFIG_BT_DEVICE_NAME_MAX];
	uint8_t mfg_id[4];        /* bytes for mfg_data[4..7]; valid with addr_valid */
};
void proxy_identity_derive(const uint8_t *hwid, ssize_t hwid_len,
			   struct proxy_identity *out); /* <6 bytes -> fallback: default name, addr_valid=false */

/* Link/advertising policy — the decisions, not the BT calls. */
struct proxy_link_state { bool connected; bool adv_active; bool link_secure; bool locked_mode; };
bool proxy_should_start_adv(const struct proxy_link_state *s);   /* !connected && !adv_active */
bool proxy_may_forward(const struct proxy_link_state *s);        /* connected && link_secure */
k_timeout_t proxy_security_window(bool locked_mode);             /* 10s/60s today; single 60s if TODO_ARCHITECTURE Task 2 lands first — test whichever is current */

/* NUS send policy. */
uint16_t proxy_nus_chunk_limit(uint16_t att_mtu);                /* mtu>3 ? mtu-3 : 20 */
enum proxy_send_verdict { PROXY_SEND_CONSUMED, PROXY_SEND_RETRY, PROXY_SEND_DROP };
enum proxy_send_verdict proxy_send_result(int bt_nus_send_err);  /* 0->CONSUMED, -ENOMEM/-EAGAIN->RETRY, else DROP */
```

`main.c` call sites change mechanically (e.g. `advertising_start()` computes
`proxy_should_start_adv` from its locals under the mutex; `ble_write_thread` uses
`proxy_nus_chunk_limit`/`proxy_send_result`). Do **not** try to extract the
mutex/work/callback orchestration — that stays in `main.c` and is covered by Phase 5.

Note: `TODO_ARCHITECTURE.md` Task 6a (move the serial-side hook out of ISR context)
is deliberately deferred to Phase 4 (it belongs with the `uart_bridge` extraction).

Dependencies: none (Phase 1 recommended first so the matrix guards this refactor).
Acceptance: all six configs compile; `check_configs.py` green; zero functional change
(pass-through hooks byte-identical); user bench-test noted as "no behavior change
expected".

## Phase 3 — Unit tests (ztest on `native_sim`)

Layout: one ztest app per suite under `tests/unit/<suite>/` with `CMakeLists.txt`,
`prj.conf` (`CONFIG_ZTEST=y` + the minimum extra config), `testcase.yaml`
(`platform_allow: [native_sim]`), and the suite compiling
`${CMAKE_CURRENT_SOURCE_DIR}/../../../src/proxy_core.c` directly (no library
packaging needed). BT *headers* (`bluetooth/addr.h` for `bt_addr_le_t`) are available
without enabling the stack; if any suite needs `CONFIG_BT` for types only, prefer
restructuring proxy_core to avoid it.

CI: `unit-tests` job runs
`west twister -T app/tests/unit -p native_sim --inline-logs` inside the same
container/workspace as Phase 1, publishing `twister-out` JUnit XML with
a test-report action.

### Test list — Group B: hooks (`tests/unit/hooks`)

| ID | Test | Notes |
|----|------|-------|
| B1 | output clamped to `out_size` when `in_len > out_size` | both hooks |
| B2 | return value == bytes copied for normal input; content byte-identical | pass-through contract |
| B3 | zero-length input → returns 0, writes nothing | |
| B4 | input exactly `PROC_BUF_SIZE` → fully copied | boundary |
| B5 | *(placeholder, activates with real hook logic)* grow path ≤ out_size honored; return-0 drops | keep as documented-skipped test until hooks are non-trivial |

### Group C: identity (`tests/unit/identity`)

| ID | Test | Notes |
|----|------|-------|
| C1 | fixed 8-byte hwid → addr = low 6 bytes with two MSBs of `addr.a.val[5]` forced (static-random), type `BT_ADDR_LE_RANDOM`, `addr_valid=true` | mirror `BT_ADDR_SET_STATIC` |
| C2 | name == `"nrfProxy-XXXX"` with `XXXX` = `%02X%02X` of hwid[5],hwid[4] | exact format |
| C3 | name fits the buffer for the max-length base name (no truncation/overflow; assert trailing NUL) | `CONFIG_BT_DEVICE_NAME_MAX=20` |
| C4 | `mfg_id` == hwid[0..3] | feeds `mfg_data[4..7]` |
| C5 | hwid_len < 6 (0, 5, and negative/error) → default name, `addr_valid=false`, mfg untouched | fallback path |
| C6 | determinism: same hwid twice → identical outputs | the reboot-stability property |

### Group D: link policy & send policy (`tests/unit/policy`)

| ID | Test | Notes |
|----|------|-------|
| D1 | `proxy_should_start_adv`: false when connected; false when adv_active; true otherwise (all 4 combos) | encodes the `recycled`/`-EALREADY` guards |
| D2 | `proxy_may_forward`: only connected && secure (all 4 combos) | encryption gate |
| D3 | `proxy_security_window`: locked vs pairing values match the defines current at implementation time | see Task-2 note in Phase 2 API |
| D4 | `proxy_nus_chunk_limit`: 23→20, 247→244, 3→20, 0→20 | MTU fallback |
| D5 | `proxy_send_result`: 0→CONSUMED; -ENOMEM→RETRY; -EAGAIN→RETRY; -ENOTCONN/-EINVAL/-EPIPE→DROP | error policy |
| D6 | **event-sequence table tests**: replay the §5 invariants as sequences over `proxy_link_state` + the decision functions: (a) disconnect→recycled→start allowed; (b) fast→slow stop (adv_active stays true)→recycled→start suppressed; (c) failed connect (connected=false, adv_active=false)→recycled→start allowed; (d) connect during fast phase → start suppressed while connected | regression armor for the three field bugs; keep them as data-driven cases so more sequences are easy to add |

Dependencies: Phase 2. Acceptance: `west twister -T tests/unit -p native_sim` green in
CI; suites also runnable in WSL locally (document the one-liner).

## Phase 4 — Extract `uart_bridge` + integration tests on `native_sim`

**4a. Extraction.** Move the UART data path out of `main.c` into `src/uart_bridge.c/h`
— everything with **no BT dependency**: `uart_rx_slab`, both ring buffers,
`uart_tx_buf`/`uart_tx_in_progress`, `uart_cb`, `uart_tx_kick`, `uart_init`,
`uart_rx_ringbuf_drain`. Interface to `main.c` (keep it this narrow):

```c
int  uart_bridge_init(const struct device *uart_dev, struct k_sem *rx_data_ready);
void uart_bridge_send(const uint8_t *data, size_t len);   /* -> TX ring + kick (logs drops) */
/* RX consumption stays direct ring access from ble_write_thread for now:        */
uint32_t uart_bridge_rx_claim(uint8_t **buf, uint32_t max);
void     uart_bridge_rx_finish(uint32_t consumed);
void     uart_bridge_rx_drain(void);
```

**4b. Task 6a lands here**: the ISR's `UART_RX_RDY` case now puts **raw** bytes in the
ring; `on_uart_rx` runs in `ble_write_thread` after claim, before `bt_nus_send`.
Follow the consequence analysis in `TODO_ARCHITECTURE.md` Task 6 verbatim (hook sees
different chunk boundaries — documented as fine; claim ≤ `max_send` then send
`proc_buf` in slices honoring RETRY semantics — once ring bytes are consumed, retries
come from `proc_buf`, never re-claim; `ring_buf_get_finish` reflects pre-hook ring
bytes). This is the one behavior-affecting step of the whole plan — flag it loudly for
user bench-testing (sustained 115200 serial→phone stream).

**4c. Integration tests** `tests/integration/uart_bridge/` on `native_sim`, driving the
real `uart_bridge.c`:

- **Option A (preferred)**: `uart_emul` device with async API (verify-before-use item
  3): DT overlay instantiates the emulator, tests inject RX bytes and capture TX.
- **Option B (fallback)**: compile `uart_bridge.c` against a small stub uart layer
  (the file's uart calls behind `#ifdef`-free indirection is ugly; prefer a
  `uart_bridge_ops` struct of function pointers defaulting to the real driver — only
  if Option A is impossible).

| ID | Test | Notes |
|----|------|-------|
| E1 | RX path: injected UART chunks appear in the RX ring intact and in order; `rx_data_ready` given | |
| E2 | RX ring overflow: inject > 2048 B without consuming → put-side truncation, no corruption; (after `TODO_ARCHITECTURE` Task 4 lands) drop counter incremented | coordinate with Task 4 if implemented |
| E3 | TX path: `uart_bridge_send` > 64 B → split into sequential ≤64 B `uart_tx` transfers chained from TX_DONE; total bytes/order preserved | the staging contract |
| E4 | concurrent kicks: TX_DONE-driven kick + thread kick → exactly one transfer in flight at all times | assert via emulator/stub bookkeeping |
| E5 | `uart_tx` start failure → staged chunk dropped, flag cleared, next send recovers the pipeline | inject failure via emul/stub |
| E6 | `UART_RX_BUF_REQUEST` slab-empty path → RX_DISABLED → automatic restart; data flows again | the recovery invariant; (after Task 3) also the retry-work path |
| E7 | drain-from-consumer: producer injects while `uart_bridge_rx_drain()` runs → no index corruption, subsequent data intact | the `ring_buf_reset` regression |
| E8 | hook-in-thread (post-4b): oversized hook output (grow to 512) sent in slices; RETRY mid-slice retries from `proc_buf` without re-claiming | encodes the 6a consequence rules |

Dependencies: Phase 2 (hooks live in proxy_core), Phase 1 (matrix guards the refactor).
Acceptance: six configs compile + `check_configs.py` green; twister integration suite
green in CI; user bench-test of both directions noted as required (behavior changed
in 4b).

## Phase 5 — BabbleSim end-to-end BLE tests  *(stretch; nightly/manual job, never blocks CI)*

Simulated nRF52 (`nrf52_bsim`) running the real firmware against a scripted central.
Setup (all inside the CI container): verify bsim modules in the workspace
(verify-before-use item 4), build BabbleSim (`bsim` repo, `make everything`), build the
app for `nrf52_bsim` (needs a bsim overlay: UART1 may need remapping/stubbing — the
data source can be the bsim UART backend or a test-only feeder thread behind
`CONFIG_*` test flag), build a companion central app (`tests/bsim/central/`) using the
Zephyr BT central APIs + NCS `bt_nus_client`, then run both against one phy
(`bs_2G4_phy_v1`) with a runner script `tests/bsim/run.sh` that asserts pass/fail from
device exit codes/output (follow the upstream `tests/bsim/bluetooth` runner patterns).

| ID | Test | What it protects |
|----|------|------------------|
| F1 | central connects (pairing mode), pairs Just Works, subscribes, round-trips data both directions | the whole happy path |
| F2 | disconnect → device advertises again → reconnect succeeds; repeat ×10 | the `recycled` restart invariant (field bug #1) |
| F3 | stay connected past 30 s idle pre-connect: observe fast→slow advertising interval change from the central's scanner | two-phase advertising + `-EALREADY` invariant (field bug #2) |
| F4 | unencrypted central (never initiates pairing) receives no NUS data and is disconnected at the watchdog window | encryption gate + watchdog |
| F5 | after bonding, a *second* central's connect attempts are ignored (accept list); first central still reconnects | locked mode |
| F6 | bonded reconnect after simulated power cycle of the peripheral (settings/NVS persistence via bsim flash file) | bond persistence + identity stability |
| F7 | throughput soak: sustained feed ≥ 60 simulated seconds, zero drops while connected/secure | flow control end-to-end |

CI: separate workflow `bsim.yml`, `on: schedule` (nightly) + `workflow_dispatch`,
`continue-on-error` initially. Budget-box this phase: if the UART-feeding problem or
bsim/NCS integration turns into a swamp, ship F1/F2 only and record the rest as
follow-ups here.

Dependencies: Phases 0–2 (Phase 4 not strictly required). Acceptance: F1+F2 green in
the nightly workflow.

## Deliberately out of scope

- Hardware-in-the-loop CI (user does hardware testing by hand; §Pre-ship checklist in
  `TODO_ARCHITECTURE.md` Task 8 stays the manual list).
- Dependabot beyond `github-actions` (nothing else applies).
- Release/flash automation and `APPROTECT`/hardening (explicitly unwanted — CLAUDE.md).
- Fixing `TODO_ARCHITECTURE.md` Tasks 2/3/4 — separate work; where a test overlaps
  (D3, E2, E6) the table says how to coordinate.

## Dependency graph & suggested order

```
Phase 0 (dependabot + CI skeleton + lint)
  └─► Phase 1 (build matrix A1–A11)          ← merge before any refactor
        └─► Phase 2 (proxy_core)             ← pure extraction, no behavior change
              └─► Phase 3 (unit B/C/D)       ← first real test coverage
              └─► Phase 4 (uart_bridge + 6a, integration E1–E8)   ← the one behavior change
                    └─► Phase 5 (bsim F1–F7, stretch, nightly)
```

One phase per PR/commit-point; suggest a commit to the user at each phase boundary
with the six-config verification results in the message.

## Definition of done (whole plan)

1. `dependabot.yml` live; CI runs on every PR: lint + six-config build matrix with
   config assertions + native_sim unit & integration suites; README badge green.
2. Breaking any documented invariant (flash offset, PM re-enabled, prod.conf stripping
   the lock, async-UART gating, adv-restart predicates, SPSC drain, TX staging) fails
   CI.
3. `main.c` reduced to Zephyr/BT glue; `proxy_core` + `uart_bridge` carry the logic
   and are unit/integration tested.
4. bsim nightly runs F1/F2 at minimum, or this file documents exactly why not and
   what's left.
5. This file updated: verify-before-use findings recorded, IDs checked off, deviations
   explained.
