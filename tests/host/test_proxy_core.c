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

/* Assemble a well-formed 16-byte device-control packet: start, length, opcode,
 * payload (zero-padded), and a correct trailing checksum. */
static void build_cmd(uint8_t pkt[PROXY_CMD_PACKET_LEN], uint8_t op,
		      const uint8_t *payload, size_t payload_len)
{
	uint8_t sum = 0;

	memset(pkt, 0, PROXY_CMD_PACKET_LEN);
	pkt[0] = PROXY_CMD_START;
	pkt[1] = PROXY_CMD_PACKET_LEN;
	pkt[2] = op;
	if (payload != NULL && payload_len > 0) {
		memcpy(&pkt[PROXY_CMD_PAYLOAD_OFF], payload, payload_len);
	}
	for (size_t i = 0; i < PROXY_CMD_PACKET_LEN - 1; i++) {
		sum = (uint8_t)(sum + pkt[i]);
	}
	pkt[PROXY_CMD_PACKET_LEN - 1] = sum;
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

	/* --- device-control command --- */
	{
		struct proxy_cmd cmd;
		uint8_t pkt[PROXY_CMD_PACKET_LEN];

		/* baud allow-list (1200 for Bafang, up to 115200) */
		expect_true("baud 1200 ok", proxy_baud_supported(1200));
		expect_true("baud 9600 ok", proxy_baud_supported(9600));
		expect_true("baud 19200 ok", proxy_baud_supported(19200));
		expect_true("baud 115200 ok", proxy_baud_supported(115200));
		expect_true("baud 250000 bad", !proxy_baud_supported(250000));
		expect_true("baud 0 bad", !proxy_baud_supported(0));

		/* relay enable: opcode 0x01, payload[0]=1 */
		build_cmd(pkt, PROXY_OP_RELAY, (uint8_t[]){ 1 }, 1);
		expect_u("relay enable kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_RELAY_ENABLE);
		expect_u("relay enable ack len", cmd.ack_len, PROXY_CMD_PACKET_LEN);
		expect_true("relay enable ack echoes",
			    memcmp(cmd.ack, pkt, PROXY_CMD_PACKET_LEN) == 0);

		/* relay disable: opcode 0x01, payload[0]=0 */
		build_cmd(pkt, PROXY_OP_RELAY, (uint8_t[]){ 0 }, 1);
		expect_u("relay disable kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_RELAY_DISABLE);

		/* relay out-of-range state -> INVALID (framed, so NOT forwarded) */
		build_cmd(pkt, PROXY_OP_RELAY, (uint8_t[]){ 5 }, 1);
		expect_u("relay bad state kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_INVALID);
		expect_u("relay bad state no ack", cmd.ack_len, 0);

		/* set baud 115200 = 0x0001C200, little-endian payload */
		build_cmd(pkt, PROXY_OP_BAUD,
			  (uint8_t[]){ 0x00, 0xC2, 0x01, 0x00 }, 4);
		expect_u("baud set kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_SET_BAUD);
		expect_u("baud set value", cmd.baud, 115200);
		expect_u("baud set ack len", cmd.ack_len, PROXY_CMD_PACKET_LEN);

		/* set baud 1200 = 0x000004B0 */
		build_cmd(pkt, PROXY_OP_BAUD,
			  (uint8_t[]){ 0xB0, 0x04, 0x00, 0x00 }, 4);
		expect_u("baud 1200 kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_SET_BAUD);
		expect_u("baud 1200 value", cmd.baud, 1200);

		/* unsupported baud 250000 = 0x0003D090 -> INVALID */
		build_cmd(pkt, PROXY_OP_BAUD,
			  (uint8_t[]){ 0x90, 0xD0, 0x03, 0x00 }, 4);
		expect_u("baud unsupported kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_INVALID);
		expect_u("baud unsupported no ack", cmd.ack_len, 0);

		/* unknown opcode, valid framing -> INVALID */
		build_cmd(pkt, 0x7F, NULL, 0);
		expect_u("unknown opcode kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_INVALID);

		/* bad checksum -> NONE (falls through to forward as data) */
		build_cmd(pkt, PROXY_OP_RELAY, (uint8_t[]){ 1 }, 1);
		pkt[PROXY_CMD_PACKET_LEN - 1] ^= 0xFF;
		expect_u("bad checksum kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_NONE);

		/* wrong start byte -> NONE */
		build_cmd(pkt, PROXY_OP_RELAY, (uint8_t[]){ 1 }, 1);
		pkt[0] = 0x59;
		expect_u("wrong start kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_NONE);

		/* wrong on-wire length -> NONE (a legacy 3-byte packet is data now) */
		{
			uint8_t shortpkt[] = { 0x22, 0x01, 0x23 };
			expect_u("wrong length kind",
				 proxy_cmd_parse(shortpkt, sizeof(shortpkt), &cmd),
				 PROXY_CMD_NONE);
		}

		/* length byte not matching PACKET_LEN -> NONE */
		build_cmd(pkt, PROXY_OP_RELAY, (uint8_t[]){ 1 }, 1);
		pkt[1] = 0x0A;   /* claims 10 bytes; checksum now wrong too */
		expect_u("length mismatch kind",
			 proxy_cmd_parse(pkt, sizeof(pkt), &cmd),
			 PROXY_CMD_NONE);
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
