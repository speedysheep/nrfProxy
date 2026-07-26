/*
 * Host tests for uart_rx_retry policy (no Zephyr required).
 * gcc -O0 -Wall -Wextra -o test_uart_rx_retry.exe test_uart_rx_retry.c ../../src/uart_rx_retry.c
 */
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>

#include "../../src/uart_rx_retry.h"

static int failures;

static void expect_true(const char *name, bool v)
{
	if (!v) {
		fprintf(stderr, "FAIL %s: expected true\n", name);
		failures++;
	} else {
		printf("ok   %s\n", name);
	}
}

static void expect_false(const char *name, bool v)
{
	if (v) {
		fprintf(stderr, "FAIL %s: expected false\n", name);
		failures++;
	} else {
		printf("ok   %s\n", name);
	}
}

int main(void)
{
	expect_true("enable 0 ok", uart_rx_enable_succeeded(0));
	expect_true("enable -EBUSY ok", uart_rx_enable_succeeded(-EBUSY));
	expect_false("enable -ENOMEM fail", uart_rx_enable_succeeded(-ENOMEM));

	expect_true("retry on alloc fail",
		    uart_rx_should_schedule_retry(false, 0));
	expect_true("retry on enable fail",
		    uart_rx_should_schedule_retry(true, -ENOMEM));
	expect_false("no retry on success",
		     uart_rx_should_schedule_retry(true, 0));
	expect_false("no retry on -EBUSY",
		     uart_rx_should_schedule_retry(true, -EBUSY));

	bool latch = false;
	expect_true("warn first", uart_rx_retry_warn_once(&latch));
	expect_false("warn second suppressed", uart_rx_retry_warn_once(&latch));
	uart_rx_retry_clear_latch(&latch);
	expect_true("warn after clear", uart_rx_retry_warn_once(&latch));

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
