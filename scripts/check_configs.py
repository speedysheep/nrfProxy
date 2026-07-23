#!/usr/bin/env python3
"""Assert post-build Kconfig / layout invariants for nrfProxy board targets.

Usage:
  python scripts/check_configs.py <target> <build_dir>
  python scripts/check_configs.py --self-test

Targets match build.ps1: dk, xiao, xiao_prod, promicro, promicro_prod, dongle.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Flash load offsets (hex strings as they appear in .config).
OFFSETS = {
    "dk": "0x0",
    "xiao": "0x27000",
    "xiao_prod": "0x27000",
    "promicro": "0x26000",
    "promicro_prod": "0x26000",
    "dongle": "0x1000",
}

PROD_TARGETS = {"xiao_prod", "promicro_prod"}


def parse_config(path: Path) -> Dict[str, Optional[str]]:
    """Parse a Zephyr .config into name -> value (None means '=n' / unset-as-n)."""
    cfg: Dict[str, Optional[str]] = {}
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            # "# CONFIG_FOO is not set"
            if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                name = line[len("# ") : -len(" is not set")]
                cfg[name] = None
            continue
        if "=" not in line:
            continue
        name, val = line.split("=", 1)
        cfg[name] = val.strip().strip('"')
    return cfg


def find_dotconfig(build_dir: Path) -> Path:
    candidates = [
        build_dir / "nrfProxy" / "zephyr" / ".config",
        build_dir / "zephyr" / ".config",
        build_dir / ".config",
    ]
    for c in candidates:
        if c.is_file():
            return c
    raise FileNotFoundError(
        f"no .config under {build_dir} (tried {[str(c) for c in candidates]})"
    )


def check_target(target: str, build_dir: Path) -> List[str]:
    errors: List[str] = []
    if target not in OFFSETS:
        return [f"unknown target {target!r}; expected one of {sorted(OFFSETS)}"]

    try:
        cfg_path = find_dotconfig(build_dir)
    except FileNotFoundError as e:
        return [str(e)]

    cfg = parse_config(cfg_path)

    def expect_y(name: str) -> None:
        if cfg.get(name) != "y":
            errors.append(f"{name}: expected y, got {cfg.get(name)!r}")

    def expect_not_y(name: str) -> None:
        if cfg.get(name) == "y":
            errors.append(f"{name}: must not be y")

    def expect_unset_or_n(name: str) -> None:
        val = cfg.get(name)
        if val == "y":
            errors.append(f"{name}: expected unset/n for prod, got y")

    # Offset
    want = OFFSETS[target]
    got = cfg.get("CONFIG_FLASH_LOAD_OFFSET")
    if got != want:
        errors.append(f"CONFIG_FLASH_LOAD_OFFSET: expected {want}, got {got!r}")

    expect_y("CONFIG_UART_1_ASYNC")
    expect_not_y("CONFIG_UART_1_INTERRUPT_DRIVEN")

    if cfg.get("CONFIG_BT_MAX_CONN") != "1":
        errors.append(
            f"CONFIG_BT_MAX_CONN: expected 1, got {cfg.get('CONFIG_BT_MAX_CONN')!r}"
        )
    if cfg.get("CONFIG_BT_MAX_PAIRED") != "1":
        errors.append(
            f"CONFIG_BT_MAX_PAIRED: expected 1, got {cfg.get('CONFIG_BT_MAX_PAIRED')!r}"
        )
    expect_y("CONFIG_BT_FILTER_ACCEPT_LIST")

    if target in PROD_TARGETS:
        expect_unset_or_n("CONFIG_LOG")
        expect_y("CONFIG_SERIAL")
        expect_y("CONFIG_BT_SMP")
        expect_y("CONFIG_BT_SETTINGS")
        expect_y("CONFIG_NVS")

    # No partitions.yml anywhere under build dir (Partition Manager off).
    for root, _dirs, files in os.walk(build_dir):
        if "partitions.yml" in files:
            errors.append(f"partitions.yml present at {Path(root) / 'partitions.yml'}")

    return errors


def write_fixture(root: Path, relative: str, content: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


def self_test() -> int:
    failures = 0

    def case(name: str, target: str, cfg: str, expect_ok: bool,
             extra_files: Optional[List[Tuple[str, str]]] = None) -> None:
        nonlocal failures
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            write_fixture(build, "nrfProxy/zephyr/.config", cfg)
            if extra_files:
                for rel, body in extra_files:
                    write_fixture(build, rel, body)
            errs = check_target(target, build)
            ok = len(errs) == 0
            if ok != expect_ok:
                print(f"FAIL {name}: ok={ok} expected_ok={expect_ok} errs={errs}")
                failures += 1
            else:
                print(f"ok   {name}")

    good_dk = """
CONFIG_FLASH_LOAD_OFFSET=0x0
CONFIG_UART_1_ASYNC=y
# CONFIG_UART_1_INTERRUPT_DRIVEN is not set
CONFIG_BT_MAX_CONN=1
CONFIG_BT_MAX_PAIRED=1
CONFIG_BT_FILTER_ACCEPT_LIST=y
CONFIG_SERIAL=y
CONFIG_LOG=y
"""
    case("good dk", "dk", good_dk, True)

    bad_offset = good_dk.replace("0x0", "0x1000")
    case("wrong offset fails", "dk", bad_offset, False)

    case(
        "partitions.yml fails",
        "dk",
        good_dk,
        False,
        extra_files=[("partitions.yml", "app:\n  address: 0x0\n")],
    )

    good_prod = """
CONFIG_FLASH_LOAD_OFFSET=0x27000
CONFIG_UART_1_ASYNC=y
# CONFIG_UART_1_INTERRUPT_DRIVEN is not set
CONFIG_BT_MAX_CONN=1
CONFIG_BT_MAX_PAIRED=1
CONFIG_BT_FILTER_ACCEPT_LIST=y
CONFIG_SERIAL=y
# CONFIG_LOG is not set
CONFIG_BT_SMP=y
CONFIG_BT_SETTINGS=y
CONFIG_NVS=y
"""
    case("good xiao_prod", "xiao_prod", good_prod, True)

    case(
        "prod with LOG=y fails",
        "xiao_prod",
        good_prod.replace("# CONFIG_LOG is not set", "CONFIG_LOG=y"),
        False,
    )

    case(
        "prod missing BT_SETTINGS fails",
        "xiao_prod",
        good_prod.replace("CONFIG_BT_SETTINGS=y\n", ""),
        False,
    )

    case(
        "async missing fails",
        "dk",
        good_dk.replace("CONFIG_UART_1_ASYNC=y\n", ""),
        False,
    )

    if failures:
        print(f"{failures} self-test failure(s)")
        return 1
    print("all self-tests passed")
    return 0


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("target", nargs="?", help="build.ps1 target name")
    ap.add_argument("build_dir", nargs="?", help="west -d build directory")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.target or not args.build_dir:
        ap.error("target and build_dir required (or pass --self-test)")

    errs = check_target(args.target, Path(args.build_dir))
    if errs:
        print(f"check_configs FAILED for {args.target} ({args.build_dir}):")
        for e in errs:
            print(f"  - {e}")
        return 1
    print(f"check_configs OK: {args.target}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
