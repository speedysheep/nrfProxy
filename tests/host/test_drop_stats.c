#include <stdio.h>
#include <stdbool.h>

#include "../../src/drop_stats.h"

static int failures;

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %u want %u\n", name, got, want);
		failures++;
	} else {
		printf("ok   %s\n", name);
	}
}

static void expect_bool(const char *name, bool got, bool want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %d want %d\n", name, got, want);
		failures++;
	} else {
		printf("ok   %s\n", name);
	}
}

int main(void)
{
	expect_u32("overflow partial", drop_stats_rx_overflow(100, 60), 40);
	expect_u32("overflow none", drop_stats_rx_overflow(100, 100), 0);
	expect_u32("overflow zero attempt", drop_stats_rx_overflow(0, 0), 0);

	expect_bool("report when changed+interval",
		    drop_stats_should_report(15000, 0, 10000, true), true);
	expect_bool("no report if unchanged",
		    drop_stats_should_report(15000, 0, 10000, false), false);
	expect_bool("no report before interval",
		    drop_stats_should_report(5000, 0, 10000, true), false);
	expect_bool("report at exact interval",
		    drop_stats_should_report(10000, 0, 10000, true), true);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
