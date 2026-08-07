# TESTING.md — what is tested, how to run it, and what isn't

Written 2026-07-29 from a direct audit of the tree and a live CI run, replacing
`ADD_TESTING_PLAN.md`. That document was a *plan* whose status section had gone badly stale —
it claimed nothing had ever been built or run, which stopped being true once CI came up. This
file describes what the project actually has, with counts taken by reading the suites rather
than quoted from memory.

**Headline: 118 automated test cases across four tiers, all gating CI.**

---

## 1. The four tiers

| Tier | Where | Cases | Needs | Runs in CI |
|------|-------|-------|-------|-----------|
| **Host** | `tests/host/` | **49** | a C compiler | `lint` job |
| **Unit (ztest)** | `tests/unit/{hooks,identity,policy}/` | **25** | NCS + `native_sim` | `unit-tests` job |
| **Integration (ztest)** | `tests/integration/uart_bridge/` | **8** | NCS + `native_sim` | `integration` job |
| **Config/script** | `scripts/test_*.py` | **36** | python3 only | `lint` job |

Plus the **six-config build matrix**, which is a test in the sense that matters here: this
project's history is dominated by configuration faults (the Partition Manager `0x0` link bug,
the `-ENOSYS` async-UART trap), so "does it still build correctly for every board" catches a
class of defect no unit test can.

### Host — `tests/host/` (49 cases)

Plain C compiled with gcc/clang, no Zephyr, no board, no SDK. **The only tier that runs
natively on the Windows dev box**, and therefore the fast inner loop.

```powershell
powershell -File tests/host/run.ps1     # ~2 seconds
```

Covers `proxy_core` (hooks, identity derivation, advertising/forwarding predicates, the flat
security window, NUS chunk sizing, send-error classification), `drop_stats` and
`uart_rx_retry`.

**`drop_stats` and `uart_rx_retry` have no other unit-level coverage.** The integration suite
links both modules but exercises them through `uart_bridge` rather than asserting on their
policy functions — so this tier is not redundant with the ztest ones, which is why it now runs
in CI rather than only on demand.

### Unit — `tests/unit/` (25 cases, `native_sim`)

`proxy_core.c` compiled straight into three ztest suites; no BLE stack, no board.

- **`hooks` (5)** — output clamped to `out_size`, byte-identical passthrough, zero-length
  input, input exactly `PROC_BUF_SIZE`, grow-and-drop paths.
- **`identity` (10)** — address is the low six hwid bytes made static-random, `-XXXX` name
  suffix format, manufacturer-ID layout, over-long and maximum-length base names, short/null/
  minimum-length hwid fallbacks, and that derivation is **deterministic** (the property that
  makes the address stable across reboots without flash) and distinct for distinct hwids.
- **`policy` (10)** — every combination of the advertising-start and forwarding predicates,
  the flat security window, NUS chunk limit, slice walking over a grown output, send-result
  classification, and two *sequence* tests replaying the advertising-restart and
  forwarding-gate orderings that produced real field bugs.

### Integration — `tests/integration/uart_bridge/` (8 cases, `native_sim`)

The real `src/uart_bridge.c` driven against Zephyr's emulated UART (`uart_emul`) — the ISR →
ring → consumer path, with `drop_stats` and `uart_rx_retry` linked in.

Data arrives intact and in order; RX spanning multiple driver buffers; ring overflow truncates
without corrupting; TX larger than the staging buffer is chained across `UART_TX_DONE`;
back-to-back sends preserve order; **drain concurrent with an active producer** (the SPSC
contract in `uart_bridge.h`, and the one rule whose violation was a real bug); drain-when-empty;
and `claim`/`finish(0)` keeping data for a later claim.

### Config and script tests — `scripts/test_*.py` (36 cases)

Stdlib `unittest`, no SDK.

- **`test_check_configs.py` (25)** — each takes a known-good synthetic `.config`, breaks
  exactly one invariant, and asserts the matching check goes red. That is the acceptance
  criterion for the build matrix, mechanised.
- **`test_assert_tests_ran.py` (11)** — tests for the guard against a zero-test pass (§3),
  covering each way a suite can appear to succeed while executing nothing.

---

## 2. Running the suites

### Locally on Windows — no container needed

```powershell
powershell -File tests/host/run.ps1                    # 49 host cases
python scripts/test_check_configs.py                   # config-checker tests
python scripts/test_assert_tests_ran.py                # guard tests
.\build.ps1                                            # six-config build matrix
python scripts/check_configs.py <target>               # invariants, after building
```

`native_sim` is **Linux-only**, so the ztest suites cannot run this way.

### Locally in Docker — the `native_sim` suites, same image as CI

```powershell
.\scripts\test_docker.ps1              # unit + integration on native_sim
.\scripts\test_docker.ps1 -Fresh       # discard the cached NCS workspace first
```

```bash
./scripts/test_docker.sh               # Linux/macOS
```

Runs in `ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.3.1` — the image CI uses — so a green run
here means what a green run there means. Requires only Docker; **not** a local NCS install,
since the container carries the toolchain. The NCS workspace lives in a named docker volume
(`nrfproxy-ncs-<rev>`) because `west update` pulls several GB: the first run is slow, later
runs reuse it.

> ⚠ **Not yet executed.** These scripts were written and reviewed on a machine with neither
> Docker nor a WSL distribution installed, so they are syntax-checked and shellchecked but
> have never run. Expect the first real run to need fixes, and record them here.

Implementation notes worth knowing if it misbehaves: the container runs as **root** because
the entry script must `apt-get install make` (the toolchain image ships cmake and ninja but
not make, which `native_sim`'s runner needs), and it hands ownership of the `twister-out-*`
directories back to the checkout owner afterwards. Both suites run even if the first fails.

### In CI

`.github/workflows/ci.yml` on every push to `main` and every pull request: `lint`,
`build-matrix` (six targets × config assertions × artifact upload), `unit-tests`, `integration`.

---

## 3. What makes a failure actually fail the build

Two holes were found and closed on 2026-07-29; both were tests that *could not fail the build*
rather than assertions that were wrong.

1. **`tests/host/` ran nowhere in CI.** The entire tier gated nothing. Now in the `lint` job.
2. **A zero-test run passed silently.** `west twister` exits 0 when it runs nothing, so a
   mistyped `-T` path, a renamed suite directory, or a `testcase.yaml` that stopped matching
   the platform filter would all have reported success. `scripts/assert_tests_ran.py` now
   asserts a floor of *executed* tests and no recorded failures, wired into both twister jobs.

   **The floors count test *configurations*, not ztest cases.** Twister's JUnit report emits
   one `<testsuite>` per scenario, so `tests/unit` reports **3** (hooks, identity, policy) and
   `tests/integration` reports **1** (uart_bridge) — not the 25 and 8 ZTEST functions inside
   them. The floors are set at those numbers, so adding a suite is free and losing one fails.
   **Raise the floor whenever you add a suite** (§7).

   *This was got wrong on the first attempt: floors of 20 and 6 were set from the ZTEST counts,
   and CI went red with both suites passing 100%. The guard behaved correctly — it is recorded
   here because the two granularities are easy to conflate.*

Otherwise: no `continue-on-error` anywhere, GitHub's `bash` shell runs `-eo pipefail`, and
`if-no-files-found: error` on the firmware artifacts means a build that silently produced
nothing fails rather than uploading an empty archive.

The guard has its own suite because a guard that gates CI while being itself unverified is
precisely the false assurance it exists to prevent.

---

## 4. Artifacts

Every CI run publishes a flashable image per board target — `dk`, `xiao`, `xiao_prod`,
`promicro`, `promicro_prod`, `dongle` — each containing `zephyr.hex` and `zephyr.uf2`, with
14-day retention. Download them from the run's summary page under **Artifacts**.

Verified on run `30420121998`: all six present, 251 KB–420 KB each.

---

## 5. What is not tested, and why

Honest list. Nothing here is an accident.

| Gap | Why | Could it change? |
|-----|-----|------------------|
| **End-to-end BLE** — connect, subscribe, notify, pair, reconnect | Needs BabbleSim: the simulator built, a bsim overlay, a companion central, a runner asserting on device exit codes. A stack whose first working version is found by iterating, not written blind | Yes — see §6 |
| **`main.c` wiring** | The policy *decisions* are unit-tested and the sequence tests replay real event orderings, but they model `main.c`'s callbacks. Nothing catches `main.c` wiring an event to the wrong callback | Only bsim closes this |
| **TX in-progress flag under concurrency** | `uart_emul` offers no hook to stall or fail a transmit mid-flight, which is what opening the thread-vs-ISR race requires | Possibly, with a purpose-built fake async UART device |
| **Slab starvation recovery** | The 4×64-byte RX slab exhausting and recovering via `uart_rx_retry` | Likely testable with `uart_emul` as-is — the most tractable gap here |
| **Relay grace window** | `relay_grace_work` and its arm/cancel from `on_disconnected`/`on_security_changed` live in `main.c` — the "`main.c` wiring" gap above in concrete form. The parse side (`proxy_cmd_parse`) is host-tested, but nothing asserts that a dropout holds the pin for 60 s, that a reconnect cancels it, or that the handler's re-check wins the cancel race | Only bsim closes it in CI. If the grace ever moves into `lock_core.c` per `LOCK_PLAN.md` B1, it becomes host-testable — which is much of the argument for doing so. Until then: `RELAY_COMMAND.md` §"Quick test", steps 6–7 |
| **Hardware behaviour** — pairing dialogs, real throughput, LED states, relay switching, actual boards | No hardware in CI, by choice | No; stays a manual checklist |
| **Power consumption** | Needs instrumentation | No |

### A test that proved nothing — removed 2026-07-29

Recorded because the failure mode is worth recognising, not because it still exists.

`src/security_timeout.h` defined `SECURITY_TIMEOUT_MS = 60000` and **nothing under `src/`
included it** — the firmware's watchdog window is `PROXY_SECURITY_WINDOW_MS` in `proxy_core.h`,
reached through `proxy_security_window_ms()`. The header's only consumer was
`tests/host/test_security_timeout.c`, so that test pinned a constant no firmware code read: it
passed while proving nothing, and would have kept passing had the real window been changed to
anything at all.

Both were deleted. Nothing was lost: the property the test was protecting — that the locked and
pairing windows stay collapsed into one, after a split window caused Android to report
"couldn't pair: incorrect PIN" — is asserted against the **real** function in
`tests/unit/policy` (`proxy_security_window_ms(true) == proxy_security_window_ms(false)`, plus
a floor of 30 s) and in `tests/host/test_proxy_core.c`. Those catch a reintroduced split
however it is spelled, where the deleted test only checked that two specific macro names were
absent from a file nobody included.

**The general lesson:** a test that imports its expected value from a header the production
code does not use is testing itself. Check what `src/` actually includes.

---

## 6. If someone picks up end-to-end BLE testing

Carried over from `ADD_TESTING_PLAN.md` so it is not re-derived from scratch:

- **Board target is `nrf52_bsim`** (`boards/native/nrf_bsim/nrf52_bsim.yaml`, no variant
  suffix). Still to confirm: whether `west list` shows the babblesim modules after a `--narrow`
  update, since that is a manifest-group question and the narrow clone may omit them.
- **`bt_nus_client` exists in NCS v3.3.1** (`include/bluetooth/services/nus_client.h`), so the
  simulated central has a ready-made NUS client — confirmed, not assumed.
- **The hard part is feeding UART data, not the BLE side.** The advertising/connection cases
  need only the BLE path and can leave the peripheral's data source silent. Ship those first —
  they cover the two advertising field bugs — and treat the UART feeder (a test-only thread
  behind a `CONFIG_*` flag is simpler than wiring a bsim UART backend) as a separate step.
- The original deferral reason — "this machine has no NCS install" — **no longer holds**: the
  SDK is at `D:\ncs\v3.3.1`, and the Docker runner (§2) gives a Linux environment regardless.

---

## 7. Changing the test setup

- **New Zephyr-free module?** Host test in the same commit. It is the cheapest tier and the
  only one that runs on the dev box.
- **New ztest suite?** Add it under `tests/unit/` or `tests/integration/` (twister discovers it
  from `testcase.yaml`) and **raise the `--min` floor** in the matching `assert_tests_ran.py`
  step in `ci.yml`, or a later deletion of it goes unnoticed. The floor counts *configurations*
  (one per suite), not ztest cases — adding a whole suite raises it by one, adding cases to an
  existing suite does not change it at all.
- **New build invariant?** Add the check to `scripts/check_configs.py` *and* a test to
  `scripts/test_check_configs.py` that breaks it deliberately. Every existing check has one.
- **New shell or PowerShell script?** Add it to the `lint` job's shellcheck / parser lists.
