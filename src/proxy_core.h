/*
 * Pure proxy logic — no Zephyr BT host calls, logging, or kernel objects.
 * Host-testable with a small stub for bt_addr_le_t when not building under Zephyr.
 */
#ifndef PROXY_CORE_H_
#define PROXY_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "security_timeout.h"

#ifdef __ZEPHYR__
#include <zephyr/bluetooth/addr.h>
#else
#ifndef BT_ADDR_LE_RANDOM
#define BT_ADDR_LE_RANDOM 0x01
#endif
typedef struct {
	uint8_t val[6];
} bt_addr_t;
typedef struct {
	uint8_t type;
	bt_addr_t a;
} bt_addr_le_t;
static inline void BT_ADDR_SET_STATIC(bt_addr_t *addr)
{
	addr->val[5] |= 0xc0;
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PROXY_PROC_BUF_SIZE 512
/* Match CONFIG_BT_DEVICE_NAME_MAX used by this app (prj.conf). */
#define PROXY_DEVICE_NAME_MAX 20

size_t on_uart_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);
size_t on_ble_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);

struct proxy_identity {
	bt_addr_le_t addr;
	bool addr_valid;
	char name[PROXY_DEVICE_NAME_MAX];
	uint8_t mfg_id[4];
};

/* <6-byte hwid → default_name kept, addr_valid=false, mfg_id untouched. */
void proxy_identity_derive(const uint8_t *hwid, ssize_t hwid_len,
			   const char *default_name, struct proxy_identity *out);

struct proxy_link_state {
	bool connected;
	bool adv_active;
	bool link_secure;
	bool locked_mode;
};

bool proxy_should_start_adv(const struct proxy_link_state *s);
bool proxy_may_forward(const struct proxy_link_state *s);

/* Flat 60 s window (Task 2) — locked_mode ignored. */
uint32_t proxy_security_window_ms(bool locked_mode);

uint16_t proxy_nus_chunk_limit(uint16_t att_mtu);

enum proxy_send_verdict {
	PROXY_SEND_CONSUMED,
	PROXY_SEND_RETRY,
	PROXY_SEND_DROP,
};

enum proxy_send_verdict proxy_send_result(int bt_nus_send_err);

#ifdef __cplusplus
}
#endif

#endif /* PROXY_CORE_H_ */
