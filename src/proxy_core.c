/*
 * proxy_core — pure logic, no Zephyr. See proxy_core.h for the rule and why.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "proxy_core.h"

/* --- Interception hooks --------------------------------------------------- */

size_t on_uart_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size)
{
	size_t n = (in_len < out_size) ? in_len : out_size;

	memcpy(out, in, n);
	return n;
}

size_t on_ble_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size)
{
	size_t n = (in_len < out_size) ? in_len : out_size;

	memcpy(out, in, n);
	return n;
}

/* --- Device-control command ----------------------------------------------- */

/* 8-bit additive checksum over the packet's bytes preceding the checksum byte.
 * Trivial on purpose: the encrypted link, not this byte, is the security
 * boundary — it only catches a corrupted or mis-framed write. */
static uint8_t cmd_checksum(const uint8_t *in)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < PROXY_CMD_PACKET_LEN - 1; i++) {
		sum = (uint8_t)(sum + in[i]);
	}
	return sum;
}

bool proxy_baud_supported(uint32_t baud)
{
	static const uint32_t supported[] = {
		1200, 9600, 19200, 38400, 57600, 115200,
	};

	for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
		if (baud == supported[i]) {
			return true;
		}
	}
	return false;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

enum proxy_cmd_kind proxy_cmd_parse(const uint8_t *in, size_t in_len,
				    struct proxy_cmd *out)
{
	const uint8_t *payload = &in[PROXY_CMD_PAYLOAD_OFF];

	out->kind = PROXY_CMD_NONE;
	out->baud = 0;
	out->ack_len = 0;

	/* Framing gate: anything that isn't exactly our 16-byte packet, with a
	 * self-consistent length and a good checksum, is treated as ordinary data
	 * and forwarded down the UART. */
	if (in_len != PROXY_CMD_PACKET_LEN ||
	    in[0] != PROXY_CMD_START ||
	    in[1] != PROXY_CMD_PACKET_LEN ||
	    in[PROXY_CMD_PACKET_LEN - 1] != cmd_checksum(in)) {
		return PROXY_CMD_NONE;
	}

	/* Framed for us: from here on it is never forwarded as data, even when
	 * the opcode or payload is bad (that becomes PROXY_CMD_INVALID). */
	switch (in[2]) {
	case PROXY_OP_RELAY:
		if (payload[0] == 0) {
			out->kind = PROXY_CMD_RELAY_DISABLE;
		} else if (payload[0] == 1) {
			out->kind = PROXY_CMD_RELAY_ENABLE;
		} else {
			out->kind = PROXY_CMD_INVALID;
		}
		break;
	case PROXY_OP_BAUD: {
		uint32_t baud = le32(payload);

		if (proxy_baud_supported(baud)) {
			out->kind = PROXY_CMD_SET_BAUD;
			out->baud = baud;
		} else {
			out->kind = PROXY_CMD_INVALID;
		}
		break;
	}
	default:
		out->kind = PROXY_CMD_INVALID;
		break;
	}

	/* Echo the exact packet back for an actionable command; an INVALID one is
	 * silently dropped (no ack), so the phone can distinguish "took effect"
	 * from "refused" by whether the echo arrives. */
	if (out->kind != PROXY_CMD_INVALID) {
		memcpy(out->ack, in, PROXY_CMD_PACKET_LEN);
		out->ack_len = PROXY_CMD_PACKET_LEN;
	}

	return out->kind;
}

/* --- Per-device identity -------------------------------------------------- */

/* The two MSBs of the top address byte mark an address static-random. Mirrors
 * Zephyr's BT_ADDR_SET_STATIC, spelled out here to keep the BT headers out. */
#define ADDR_STATIC_MSBS 0xc0

void proxy_identity_derive(const char *base_name, const uint8_t *hwid,
			   int hwid_len, struct proxy_identity *out)
{
	size_t base_len = strlen(base_name);

	/* Truncate rather than overflow. main.c BUILD_ASSERTs that the real
	 * name plus its suffix fits, so this only bites a caller passing an
	 * over-long base name — a unit test, or a future rename. */
	if (base_len > sizeof(out->name) - 1) {
		base_len = sizeof(out->name) - 1;
	}

	/* Zero the whole buffer, not just the terminator: it makes the
	 * derivation byte-deterministic rather than only string-deterministic,
	 * so the tail can't carry whatever the caller's memory happened to hold
	 * into advertising data. */
	memset(out->name, 0, sizeof(out->name));
	memcpy(out->name, base_name, base_len);
	out->addr_valid = false;

	if (hwid == NULL || hwid_len < PROXY_HWID_MIN_LEN) {
		return;
	}

	/* Fixed static-random address from the low 6 bytes of the chip id. */
	memcpy(out->addr, hwid, PROXY_ADDR_LEN);
	out->addr[PROXY_ADDR_LEN - 1] |= ADDR_STATIC_MSBS;

	/* Per-device id for the manufacturer advertising data. */
	memcpy(out->mfg_id, hwid, PROXY_MFG_ID_LEN);

	out->addr_valid = true;

	/* Unique name suffix from the top id bytes (e.g. "nrfProxy-3F7A"). */
	if (base_len + sizeof("-XXXX") <= sizeof(out->name)) {
		snprintf(out->name + base_len, sizeof("-XXXX"), "-%02X%02X",
			 hwid[5], hwid[4]);
	}
}

/* --- Link / advertising policy -------------------------------------------- */

bool proxy_should_start_adv(const struct proxy_link_state *state)
{
	return !state->connected && !state->adv_active;
}

bool proxy_may_forward(const struct proxy_link_state *state)
{
	return state->connected && state->link_secure;
}

uint32_t proxy_security_window_ms(bool locked_mode)
{
	/* One window for both modes now — see PROXY_SECURITY_WINDOW_MS for why
	 * locked mode can no longer use the short one. The parameter stays so the
	 * call site keeps reading naturally and the tests can pin that the two
	 * modes agree. */
	(void)locked_mode;
	return PROXY_SECURITY_WINDOW_MS;
}

/* --- NUS send policy ------------------------------------------------------ */

uint16_t proxy_nus_chunk_limit(uint16_t att_mtu)
{
	if (att_mtu > PROXY_ATT_HEADER_LEN) {
		return (uint16_t)(att_mtu - PROXY_ATT_HEADER_LEN);
	}
	return PROXY_NUS_CHUNK_FALLBACK;
}

size_t proxy_next_slice(size_t out_len, size_t sent, uint16_t max_send)
{
	size_t remaining;

	if (sent >= out_len) {
		return 0;
	}

	remaining = out_len - sent;
	return (remaining < max_send) ? remaining : max_send;
}

enum proxy_send_verdict proxy_send_result(int bt_nus_send_err)
{
	switch (bt_nus_send_err) {
	case 0:
		return PROXY_SEND_CONSUMED;
	case -ENOMEM:
	case -EAGAIN:
		return PROXY_SEND_RETRY;
	default:
		return PROXY_SEND_DROP;
	}
}
