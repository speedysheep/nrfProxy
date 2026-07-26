/*
 * Host-side check for the security-watchdog constant (no Zephyr/NCS required).
 * Build & run:  gcc -o test_security_timeout test_security_timeout.c && ./test_security_timeout
 *           or: cl /Fe:test_security_timeout.exe test_security_timeout.c && test_security_timeout.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/security_timeout.h"

static int failures;

static void expect_eq(const char *name, long got, long want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %ld want %ld\n", name, got, want);
		failures++;
	} else {
		printf("ok   %s\n", name);
	}
}

int main(void)
{
	expect_eq("SECURITY_TIMEOUT_MS", SECURITY_TIMEOUT_MS, 60000);

	/* Guard against reintroducing a split locked/pairing window in the header. */
#ifdef SECURITY_TIMEOUT_LOCKED
	fprintf(stderr, "FAIL SECURITY_TIMEOUT_LOCKED must not exist\n");
	failures++;
#else
	printf("ok   no SECURITY_TIMEOUT_LOCKED\n");
#endif
#ifdef SECURITY_TIMEOUT_PAIRING
	fprintf(stderr, "FAIL SECURITY_TIMEOUT_PAIRING must not exist\n");
	failures++;
#else
	printf("ok   no SECURITY_TIMEOUT_PAIRING\n");
#endif

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
