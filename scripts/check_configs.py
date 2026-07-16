#!/usr/bin/env python3
"""Assert the build-configuration invariants of every nrfProxy target.

Each check here mechanises a rule that was previously enforced only by a comment
in CLAUDE.md and a human grepping build*/nrfProxy/zephyr/.config. Every one of
them has a bug behind it -- see ARCHITECTURE.md sections 5 and 7:

  A7   flash offset per board. Partition Manager once linked the Pro Micro at
       0x0 while the UF2 step still copied it to 0x26000: boot loop on every
       reset.
  A8   uart1 keeps the async API. UART_1_INTERRUPT_DRIVEN defaults y as soon as
       anything enables CONFIG_UART_INTERRUPT_DRIVEN (the USB CDC-ACM console
       does), which silently drops uart1's async API -> uart_callback_set()
       returns -ENOSYS at runtime, on those boards only.
  A9   prod.conf strips logging and the USB console but must not strip the
       serial driver or the pairing lock -- production needs the lock most.
  A10  no partitions.yml: proof Partition Manager really is disabled and the
       layout comes from devicetree.
  A11  the pairing lock's Kconfig set: one connection, one bond, accept list.

Usage:
    python scripts/check_configs.py                    # all six targets
    python scripts/check_configs.py dk xiao_prod       # named targets
    python scripts/check_configs.py dk --build-dir /tmp/scratch
    python scripts/check_configs.py --proj /path/to/nrfProxy

Exits non-zero if any check fails. Python 3 stdlib only, so it runs both in CI
and next to build.ps1 on a Windows box (the NCS toolchain bundles python).
"""

import argparse
import os
import re
import sys

# Target table, keyed the same as build.ps1 / build.sh. `offset` is the address
# the image must link at: DK has no bootloader; the XIAO and Pro Micro take
# theirs from the devicetree code_partition (their bootloaders reserve the space
# below); the dongle is the one board where it comes from the board Kconfig's
# CONFIG_FLASH_LOAD_OFFSET instead, leaving room for the Nordic USB bootloader.
TARGETS = {
    "dk": {
        "board": "nrf52840dk/nrf52840",
        "build_dir": "build_devkit",
        "offset": 0x0,
        "prod": False,
    },
    "xiao": {
        "board": "xiao_ble/nrf52840",
        "build_dir": "build_xiao",
        "offset": 0x27000,
        "prod": False,
    },
    "xiao_prod": {
        "board": "xiao_ble/nrf52840",
        "build_dir": "build_xiao_prod",
        "offset": 0x27000,
        "prod": True,
    },
    "promicro": {
        "board": "promicro_nrf52840/nrf52840/uf2",
        "build_dir": "build_promicro",
        "offset": 0x26000,
        "prod": False,
    },
    "promicro_prod": {
        "board": "promicro_nrf52840/nrf52840/uf2",
        "build_dir": "build_promicro_prod",
        "offset": 0x26000,
        "prod": True,
    },
    "dongle": {
        "board": "nrf52840dongle/nrf52840",
        "build_dir": "build_dongle",
        "offset": 0x1000,
        "prod": False,
    },
}

# The application image's .config, relative to the build dir. sysbuild is kept
# (only Partition Manager is off), so the app lives in its own image subdir.
CONFIG_PATH = os.path.join("nrfProxy", "zephyr", ".config")

_ASSIGNMENT = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")


class CheckError(Exception):
    """A check could not run at all (missing build dir, unreadable .config)."""


def load_config(path):
    """Parse a Kconfig .config into {symbol: raw value}.

    Disabled symbols are written as `# CONFIG_X is not set`, so they are simply
    absent from the result -- callers test for "not set" with `not in`.
    """
    values = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                match = _ASSIGNMENT.match(line.strip())
                if match:
                    values[match.group(1)] = match.group(2)
    except (IOError, OSError) as err:
        raise CheckError("cannot read {}: {}".format(path, err))

    if not values:
        raise CheckError("{} has no CONFIG_ assignments".format(path))
    return values


def find_partitions_yml(build_dir):
    """Every partitions.yml under build_dir (its presence means PM ran)."""
    hits = []
    for dirpath, _dirnames, filenames in os.walk(build_dir):
        if "partitions.yml" in filenames:
            hits.append(os.path.join(dirpath, "partitions.yml"))
    return hits


def _describe(values, symbol):
    if symbol in values:
        return "{}={}".format(symbol, values[symbol])
    return "{} is not set".format(symbol)


def check_target(name, spec, build_dir):
    """Run every check for one target. Returns [(id, description, ok, detail)]."""
    results = []

    def expect_value(check_id, description, symbol, expected):
        actual = values.get(symbol)
        results.append((check_id, description, actual == expected,
                        _describe(values, symbol)))

    def expect_unset(check_id, description, symbol):
        results.append((check_id, description, symbol not in values,
                        _describe(values, symbol)))

    values = load_config(os.path.join(build_dir, CONFIG_PATH))

    # A7 -- flash offset. An unset symbol means the default, 0.
    raw_offset = values.get("CONFIG_FLASH_LOAD_OFFSET", "0")
    try:
        offset = int(raw_offset, 0)
    except ValueError:
        offset = None
    results.append((
        "A7", "links at 0x{:x}".format(spec["offset"]),
        offset == spec["offset"],
        "CONFIG_FLASH_LOAD_OFFSET={} (0x{:x})".format(raw_offset, offset)
        if offset is not None else
        "CONFIG_FLASH_LOAD_OFFSET={} (unparseable)".format(raw_offset),
    ))

    # A8 -- uart1 keeps the async API on every board.
    expect_value("A8", "uart1 async API", "CONFIG_UART_1_ASYNC", "y")
    expect_unset("A8", "uart1 not interrupt-driven",
                 "CONFIG_UART_1_INTERRUPT_DRIVEN")

    # A11 -- the pairing lock: one connection, one bond, link-layer filtering.
    expect_value("A11", "single connection", "CONFIG_BT_MAX_CONN", "1")
    expect_value("A11", "single bond", "CONFIG_BT_MAX_PAIRED", "1")
    expect_value("A11", "filter accept list", "CONFIG_BT_FILTER_ACCEPT_LIST", "y")

    # A9 -- what prod.conf may and may not remove. The lock's persistence path
    # (SMP -> settings -> NVS) is asserted on every build, prod included: that
    # is the whole point of the invariant.
    expect_value("A9", "pairing/SMP", "CONFIG_BT_SMP", "y")
    expect_value("A9", "bond persistence", "CONFIG_BT_SETTINGS", "y")
    expect_value("A9", "bond storage backend", "CONFIG_NVS", "y")
    expect_value("A9", "serial driver", "CONFIG_SERIAL", "y")
    if spec["prod"]:
        expect_unset("A9", "logging stripped", "CONFIG_LOG")
        expect_unset("A9", "USB console stripped",
                     "CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM")
    else:
        # The mirror image: prod.conf must not leak into a debug build.
        expect_value("A9", "logging kept", "CONFIG_LOG", "y")

    # A10 -- Partition Manager is off, so it emitted no layout.
    stray = find_partitions_yml(build_dir)
    results.append((
        "A10", "no partitions.yml (Partition Manager off)", not stray,
        "found: {}".format(", ".join(stray)) if stray else "none under build dir",
    ))

    return results


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Assert the nrfProxy build-configuration invariants.")
    parser.add_argument(
        "targets", nargs="*", metavar="TARGET", choices=list(TARGETS),
        default=[],
        help="targets to check (default: all of {})".format(
            ", ".join(TARGETS)))
    parser.add_argument(
        "--proj", default=os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))),
        help="project root holding the build dirs (default: this repo)")
    parser.add_argument(
        "--build-dir",
        help="check this build dir instead of the target's default; "
             "only valid with exactly one target")
    args = parser.parse_args(argv)

    targets = args.targets or list(TARGETS)
    if args.build_dir and len(targets) != 1:
        parser.error("--build-dir needs exactly one target")

    failures = 0
    for name in targets:
        spec = TARGETS[name]
        build_dir = args.build_dir or os.path.join(args.proj, spec["build_dir"])
        print("== {} ({}) -- {}".format(name, spec["board"], build_dir))

        try:
            results = check_target(name, spec, build_dir)
        except CheckError as err:
            print("   ERROR  {}".format(err))
            failures += 1
            continue

        for check_id, description, ok, detail in results:
            print("   {}  {:<4} {:<34} {}".format(
                "PASS" if ok else "FAIL", check_id, description, detail))
            if not ok:
                failures += 1

    print("")
    if failures:
        print("FAILED: {} check(s) across {} target(s)".format(
            failures, len(targets)))
        return 1
    print("OK: all checks passed for {} target(s)".format(len(targets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
