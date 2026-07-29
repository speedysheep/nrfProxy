#!/usr/bin/env python3
"""Tests for assert_tests_ran.py, run against synthetic JUnit reports.

assert_tests_ran.py exists to catch a *false green* -- Twister exiting 0 having
run nothing -- so the thing that matters is that it goes red for each way that
can happen. These tests are that check, mechanised: each one hands it a report
representing one failure mode and asserts the exit code.

A guard that gates CI but is itself unverified is exactly the kind of
false assurance it was written to prevent, hence this file. Needs no SDK and no
build, so it runs in the lint job next to test_check_configs.py.

    python scripts/test_assert_tests_ran.py
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import assert_tests_ran  # noqa: E402  (needs the path above)


def report(*suites):
    """Build a JUnit <testsuites> document from (tests, failures, errors,
    skipped) tuples, the shape Twister writes."""
    body = "".join(
        '<testsuite name="s{}" tests="{}" failures="{}" errors="{}" '
        'skipped="{}"/>'.format(i, *s) for i, s in enumerate(suites))
    return "<testsuites>{}</testsuites>".format(body)


class AssertTestsRanTest(unittest.TestCase):
    def run_on(self, text, *args):
        """Write `text` to a temp report and return assert_tests_ran's exit
        code. Passing None writes no file at all."""
        path = os.path.join(self.tmp.name, "twister.xml")
        if text is not None:
            with open(path, "w") as handle:
                handle.write(text)
        return assert_tests_ran.main([path] + list(args))

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    # --- the healthy case -------------------------------------------------

    def test_passes_when_enough_tests_ran_clean(self):
        code = self.run_on(report((15, 0, 0, 0), (10, 0, 0, 0)), "--min", "20")
        self.assertEqual(code, 0, "25 clean tests should satisfy a floor of 20")

    def test_counts_are_summed_across_suites(self):
        """Twister emits one <testsuite> per suite and does not reliably
        populate the root's totals, so the sum is what must be trusted."""
        self.assertEqual(
            self.run_on(report((7, 0, 0, 0), (7, 0, 0, 0), (7, 0, 0, 0)),
                        "--min", "21"), 0)

    # --- the false green this exists to catch -----------------------------

    def test_fails_when_no_tests_ran(self):
        """The headline case: Twister exits 0 having run nothing."""
        self.assertEqual(self.run_on(report(), "--min", "20"), 1)

    def test_fails_when_below_the_floor(self):
        """A whole suite disappearing, e.g. a renamed directory."""
        self.assertEqual(self.run_on(report((5, 0, 0, 0)), "--min", "20"), 1)

    def test_fails_when_everything_was_skipped(self):
        """The realistic way this breaks: a platform filter stops matching, so
        the tests are present in the report but none of them executed."""
        self.assertEqual(self.run_on(report((25, 0, 0, 24)), "--min", "20"), 1)

    def test_skipped_tests_do_not_count_toward_the_floor(self):
        """The floor is on *executed* tests, so it sits exactly at 25 - 5 = 20:
        one more skip drops below it. Pinning both sides of the boundary is what
        makes "skipped doesn't count" a real assertion rather than a comment."""
        self.assertEqual(self.run_on(report((25, 0, 0, 5)), "--min", "20"), 0)
        self.assertEqual(self.run_on(report((25, 0, 0, 6)), "--min", "20"), 1)

    # --- ordinary failures ------------------------------------------------

    def test_fails_on_recorded_failures(self):
        self.assertEqual(self.run_on(report((25, 2, 0, 0)), "--min", "20"), 1)

    def test_fails_on_recorded_errors(self):
        self.assertEqual(self.run_on(report((25, 0, 1, 0)), "--min", "20"), 1)

    # --- the report itself being wrong ------------------------------------

    def test_fails_when_the_report_is_missing(self):
        """Twister never got far enough to write one."""
        self.assertEqual(self.run_on(None, "--min", "20"), 1)

    def test_fails_when_the_report_is_not_xml(self):
        self.assertEqual(self.run_on("this is not xml", "--min", "20"), 1)

    def test_default_floor_is_one(self):
        """Without --min, an empty report must still fail."""
        self.assertEqual(self.run_on(report()), 1)
        self.assertEqual(self.run_on(report((1, 0, 0, 0))), 0)


if __name__ == "__main__":
    unittest.main()
