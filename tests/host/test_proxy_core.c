/*
 * Host unit tests for proxy_core (no Zephyr).
 * gcc -O0 -Wall -Wextra -o test_proxy_core.exe test_proxy_core.c ../../src/proxy_core.c
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include "../../src/proxy_core.h"

static int failures;

static void expect_true(const char *n, bool v)
{
	if (!v) {
		fprintf(stderr, "FAIL %s\n", n);
		failures++;
	} else {
		printf("ok   %s\n", n);
	}
}

static void expect_u(const char *n, unsigned long got, unsigned long want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %lu want %lu\n", n, got, want);
		failures++;
	} else {
		printf("ok   %s\n", n);
	}
}

static void expect_str(const char *n, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", n, got, want);
		failures++;
	} else {
		printf("ok   %s\n", n);
	}
}

int main(void)
{
	uint8_t out[PROXY_PROC_BUF_SIZE];
	uint8_t in[PROXY_PROC_BUF_SIZE];
	size_t n;

	/* --- hooks --- */
	memset(in, 0xA5, sizeof(in));
	n = on_uart_rx(in, 10, out, 4);
	expect_u("hook clamp", n, 4);
	n = on_uart_rx(in, 8, out, sizeof(out));
	expect_u("hook passthrough len", n, 8);
	expect_true("hook passthrough data", memcmp(in, out, 8) == 0);
	expect_u("hook zero", on_uart_rx(in, 0, out, sizeof(out)), 0);
	n = on_uart_rx(in, PROXY_PROC_BUF_SIZE, out, PROXY_PROC_BUF_SIZE);
	expect_u("hook full buf", n, PROXY_PROC_BUF_SIZE);
	n = on_ble_rx(in, 5, out, sizeof(out));
	expect_u("ble hook", n, 5);

	/* --- identity --- */
	{
		uint8_t hwid[8] = { 0x11, 0x22, 0x33, 0x44, 0x7A, 0x3F, 0x00, 0x00 };
		struct proxy_identity id;

		proxy_identity_derive(hwid, 8, "nrfProxy", &id);
		expect_true("addr_valid", id.addr_valid);
		expect_true("static MSBs", (id.addr.a.val[5] & 0xc0) == 0xc0);
		expect_str("name suffix", id.name, "nrfProxy-3F7A");
		expect_true("mfg_id", memcmp(id.mfg_id, hwid, 4) == 0);

		proxy_identity_derive(hwid, 8, "nrfProxy", &id);
		struct proxy_identity id2;
		proxy_identity_derive(hwid, 8, "nrfProxy", &id2);
		expect_true("deterministic",
			    memcmp(&id, &id2, sizeof(id)) == 0);

		proxy_identity_derive(hwid, 5, "nrfProxy", &id);
		expect_true("short hwid invalid", !id.addr_valid);
		expect_str("short hwid name", id.name, "nrfProxy");
	}

	/* --- policy --- */
	{
		struct proxy_link_state s = {0};

		expect_true("start when idle", proxy_should_start_adv(&s));
		s.connected = true;
		expect_true("no start when connected", !proxy_should_start_adv(&s));
		s.connected = false;
		s.adv_active = true;
		expect_true("no start when adv", !proxy_should_start_adv(&s));

		s.connected = true;
		s.link_secure = true;
		expect_true("may forward", proxy_may_forward(&s));
		s.link_secure = false;
		expect_true("no forward unsecure", !proxy_may_forward(&s));

		expect_u("security window", proxy_security_window_ms(true), 60000);
		expect_u("security window pairing",
			 proxy_security_window_ms(false), 60000);

		expect_u("mtu 23", proxy_nus_chunk_limit(23), 20);
		expect_u("mtu 247", proxy_nus_chunk_limit(247), 244);
		expect_u("mtu 3", proxy_nus_chunk_limit(3), 20);
		expect_u("mtu 0", proxy_nus_chunk_limit(0), 20);

		expect_u("send 0", proxy_send_result(0), PROXY_SEND_CONSUMED);
		expect_u("send ENOMEM", proxy_send_result(-ENOMEM), PROXY_SEND_RETRY);
		expect_u("send EAGAIN", proxy_send_result(-EAGAIN), PROXY_SEND_RETRY);
		expect_u("send ENOTCONN", proxy_send_result(-ENOTCONN), PROXY_SEND_DROP);
	}

	/* --- event-sequence table (adv start decisions) --- */
	{
		struct proxy_link_state s;

		/* disconnect → recycled → start allowed */
		s = (struct proxy_link_state){ .connected = false, .adv_active = false };
		expect_true("seq disconnect recycled", proxy_should_start_adv(&s));

		/* fast→slow: adv_active stays true across stop→start gap → suppress */
		s = (struct proxy_link_state){ .connected = false, .adv_active = true };
		expect_true("seq fast-slow recycled", !proxy_should_start_adv(&s));

		/* failed connect → recycled → start allowed */
		s = (struct proxy_link_state){ .connected = false, .adv_active = false };
		expect_true("seq failed connect", proxy_should_start_adv(&s));

		/* connected during fast → suppress */
		s = (struct proxy_link_state){ .connected = true, .adv_active = false };
		expect_true("seq connected", !proxy_should_start_adv(&s));
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
