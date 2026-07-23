#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "proxy_core.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

size_t on_uart_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size)
{
	size_t n = MIN(in_len, out_size);

	if (n > 0) {
		memcpy(out, in, n);
	}
	return n;
}

size_t on_ble_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size)
{
	size_t n = MIN(in_len, out_size);

	if (n > 0) {
		memcpy(out, in, n);
	}
	return n;
}

void proxy_identity_derive(const uint8_t *hwid, ssize_t hwid_len,
			   const char *default_name, struct proxy_identity *out)
{
	size_t base_len;

	memset(out, 0, sizeof(*out));
	base_len = strnlen(default_name, PROXY_DEVICE_NAME_MAX - 1);
	memcpy(out->name, default_name, base_len);
	out->name[base_len] = '\0';

	if (hwid_len < 6 || hwid == NULL) {
		out->addr_valid = false;
		return;
	}

	out->addr.type = BT_ADDR_LE_RANDOM;
	memcpy(out->addr.a.val, hwid, 6);
	BT_ADDR_SET_STATIC(&out->addr.a);
	out->addr_valid = true;

	/* "-XXXX" from hwid[5], hwid[4] — leave room for NUL. */
	if (base_len + 5 < PROXY_DEVICE_NAME_MAX) {
		snprintf(out->name + base_len, PROXY_DEVICE_NAME_MAX - base_len,
			 "-%02X%02X", hwid[5], hwid[4]);
	}

	memcpy(out->mfg_id, hwid, 4);
}

bool proxy_should_start_adv(const struct proxy_link_state *s)
{
	return !s->connected && !s->adv_active;
}

bool proxy_may_forward(const struct proxy_link_state *s)
{
	return s->connected && s->link_secure;
}

uint32_t proxy_security_window_ms(bool locked_mode)
{
	(void)locked_mode;
	return SECURITY_TIMEOUT_MS;
}

uint16_t proxy_nus_chunk_limit(uint16_t att_mtu)
{
	return (att_mtu > 3) ? (uint16_t)(att_mtu - 3) : 20;
}

enum proxy_send_verdict proxy_send_result(int bt_nus_send_err)
{
	if (bt_nus_send_err == 0) {
		return PROXY_SEND_CONSUMED;
	}
	if (bt_nus_send_err == -ENOMEM || bt_nus_send_err == -EAGAIN) {
		return PROXY_SEND_RETRY;
	}
	return PROXY_SEND_DROP;
}
