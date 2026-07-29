#!/usr/bin/env python3
"""Assert a Twister run actually executed tests, and that none of them failed.

Twister already exits non-zero when a test fails, so this is not a second
opinion on failures -- it exists for the case Twister reports as success:
**zero tests ran**. If a -T path is mistyped, a suite directory is renamed, or a
testcase.yaml stops matching the platform filter, Twister can exit 0 having run
nothing at all and CI goes green. That is a worse failure than a red build,
because it looks like coverage.

So this asserts a floor: at least MIN tests present in the JUnit report, and no
failures or errors recorded in it.

Usage:
    python3 scripts/assert_tests_ran.py twister-out/twister.xml --min 20

Exits non-zero if the report is missing, unparseable, short of the floor, or
records any failure/error. Python 3 stdlib only, so it runs in the toolchain
container and next to build.ps1 on Windows.
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET


def summarise(path):
    """Return (tests, failures, errors, skipped) summed over all testsuites.

    Twister writes JUnit XML: a <testsuites> root whose <testsuite> children
    carry the counts as attributes. Sum the children rather than trusting the
    root's own attributes, which Twister does not always populate.
    """
    root = ET.parse(path).getroot()

    suites = root.iter("testsuite")
    tests = failures = errors = skipped = 0

    for suite in suites:
        tests += int(suite.get("tests", 0))
        failures += int(suite.get("failures", 0))
        errors += int(suite.get("errors", 0))
        skipped += int(suite.get("skipped", 0))

    return tests, failures, errors, skipped


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Assert a Twister run executed tests and none failed.")
    parser.add_argument("report", help="path to twister.xml (JUnit XML)")
    parser.add_argument(
        "--min", type=int, default=1, metavar="N",
        help="minimum number of tests that must appear (default: 1)")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.report):
        print("FAIL: no test report at {} -- did Twister run at all?".format(
            args.report), file=sys.stderr)
        return 1

    try:
        tests, failures, errors, skipped = summarise(args.report)
    except ET.ParseError as exc:
        print("FAIL: {} is not parseable XML: {}".format(args.report, exc),
              file=sys.stderr)
        return 1

    executed = tests - skipped

    print("{}: {} tests ({} skipped), {} failures, {} errors".format(
        args.report, tests, skipped, failures, errors))

    if executed < args.min:
        print("FAIL: only {} test(s) executed, expected at least {}. A suite "
              "that runs nothing reports success -- check the -T path and the "
              "platform filter.".format(executed, args.min), file=sys.stderr)
        return 1

    if failures or errors:
        print("FAIL: {} failure(s), {} error(s) in the report".format(
            failures, errors), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
