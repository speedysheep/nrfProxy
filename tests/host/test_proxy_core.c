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
	uint8_t out[PROC_BUF_SIZE];
	uint8_t in[PROC_BUF_SIZE];
	size_t n;

	/* --- hooks --- */
	memset(in, 0xA5, sizeof(in));
	n = on_uart_rx(in, 10, out, 4);
	expect_u("hook clamp", n, 4);
	n = on_uart_rx(in, 8, out, sizeof(out));
	expect_u("hook passthrough len", n, 8);
	expect_true("hook passthrough data", memcmp(in, out, 8) == 0);
	expect_u("hook zero", on_uart_rx(in, 0, out, sizeof(out)), 0);
	n = on_uart_rx(in, PROC_BUF_SIZE, out, PROC_BUF_SIZE);
	expect_u("hook full buf", n, PROC_BUF_SIZE);
	n = on_ble_rx(in, 5, out, sizeof(out));
	expect_u("ble hook", n, 5);

	/* --- relay command --- */
	{
		uint8_t ack[PROXY_RELAY_PACKET_LEN];
		size_t ack_len;
		enum proxy_relay_action a;

		/* enable: 0x22 0x01 0x23 */
		uint8_t en[] = { 0x22, 0x01, 0x23 };
		memset(ack, 0, sizeof(ack));
		ack_len = 0;
		a = proxy_relay_parse(en, sizeof(en), ack, sizeof(ack), &ack_len);
		expect_u("relay enable action", a, PROXY_RELAY_ENABLE);
		expect_u("relay enable ack len", ack_len, PROXY_RELAY_PACKET_LEN);
		expect_true("relay enable ack echoes", memcmp(ack, en, 3) == 0);

		/* disable: 0x22 0x00 0x22 */
		uint8_t di[] = { 0x22, 0x00, 0x22 };
		a = proxy_relay_parse(di, sizeof(di), ack, sizeof(ack), &ack_len);
		expect_u("relay disable action", a, PROXY_RELAY_DISABLE);
		expect_true("relay disable ack echoes", memcmp(ack, di, 3) == 0);

		/* bad checksum -> NONE (falls through to forward) */
		uint8_t badck[] = { 0x22, 0x01, 0x99 };
		a = proxy_relay_parse(badck, sizeof(badck), ack, sizeof(ack), &ack_len);
		expect_u("relay bad checksum", a, PROXY_RELAY_NONE);

		/* wrong start byte -> NONE */
		uint8_t badstart[] = { 0x59, 0x01, 0x5A };
		a = proxy_relay_parse(badstart, sizeof(badstart), ack, sizeof(ack), &ack_len);
		expect_u("relay wrong start", a, PROXY_RELAY_NONE);

		/* wrong length -> NONE (start+checksum-shaped but 4 bytes) */
		uint8_t longpkt[] = { 0x22, 0x01, 0x23, 0x00 };
		a = proxy_relay_parse(longpkt, sizeof(longpkt), ack, sizeof(ack), &ack_len);
		expect_u("relay wrong length", a, PROXY_RELAY_NONE);

		/* out-of-range state, checksum-valid -> NONE */
		uint8_t badstate[] = { 0x22, 0x05, 0x27 };
		a = proxy_relay_parse(badstate, sizeof(badstate), ack, sizeof(ack), &ack_len);
		expect_u("relay bad state", a, PROXY_RELAY_NONE);
	}

	/* --- identity --- */
	{
		uint8_t hwid[8] = { 0x11, 0x22, 0x33, 0x44, 0x7A, 0x3F, 0x00, 0x00 };
		struct proxy_identity id;

		proxy_identity_derive("nrfProxy", hwid, 8, &id);
		expect_true("addr_valid", id.addr_valid);
		expect_true("static MSBs", (id.addr[5] & 0xc0) == 0xc0);
		expect_str("name suffix", id.name, "nrfProxy-3F7A");
		expect_true("mfg_id", memcmp(id.mfg_id, hwid, 4) == 0);

		proxy_identity_derive("nrfProxy", hwid, 8, &id);
		struct proxy_identity id2;
		proxy_identity_derive("nrfProxy", hwid, 8, &id2);
		expect_true("deterministic",
			    memcmp(&id, &id2, sizeof(id)) == 0);

		proxy_identity_derive("nrfProxy", hwid, 5, &id);
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
