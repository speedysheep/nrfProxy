/*
 * proxy_core — the parts of nrfProxy that are decisions rather than plumbing.
 *
 * main.c owns every Zephyr and Bluetooth API call, all the locking, and all the
 * callbacks; this file owns the logic they decide with. The split exists so the
 * logic can be unit-tested (tests/unit/) without a BLE stack, a UART, or a
 * board.
 *
 * The binding rule, and the reason the types below look the way they do:
 * **nothing here may depend on Zephyr.** No kernel objects, no BT host calls, no
 * logging, no Kconfig symbols, no statics. So an address crosses the boundary as
 * six plain bytes rather than a bt_addr_le_t, and a timeout as milliseconds
 * rather than a k_timeout_t; main.c converts at the call site and BUILD_ASSERTs
 * the couplings. That keeps the suites buildable on native_sim with no BT
 * config, and compilable straight on a host with plain gcc.
 */
#ifndef PROXY_CORE_H_
#define PROXY_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Interception hooks --------------------------------------------------- */
/*
 * Called for each chunk of data as it is received, before it is forwarded on.
 * Right now they copy the input straight through, unmodified. To inspect,
 * modify, filter, or append to the data, edit the bodies: write whatever you
 * want forwarded into `out` (up to `out_size` bytes) and return how many bytes
 * you wrote. Return 0 to drop the data entirely.
 *
 * `out` is a separate scratch buffer (not the receive buffer), so you can grow
 * the data up to PROC_BUF_SIZE. Bump PROC_BUF_SIZE if you need more headroom.
 *
 * ⚠ on_uart_rx runs in ISR context (the UART async callback) — keep it light.
 * on_ble_rx runs in the Bluetooth RX thread.
 *
 * Both see transport chunks — whatever the UART's DMA delivered per
 * idle-timeout, or one GATT write — not framed application messages; add
 * reassembly if the data is message-oriented.
 */
#define PROC_BUF_SIZE 512

/* Serial -> phone: bytes received on UART1, before they go out over BLE. */
size_t on_uart_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);

/* Phone -> serial: bytes received over BLE, before they go out UART1. */
size_t on_ble_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);

/* --- Per-device identity -------------------------------------------------- */

#define PROXY_ADDR_LEN         6   /* == BT_ADDR_SIZE */
#define PROXY_MFG_ID_LEN       4   /* per-device id in the manufacturer AD */
#define PROXY_DEVICE_NAME_MAX 20   /* <= CONFIG_BT_DEVICE_NAME_MAX + 1 */
#define PROXY_HWID_MIN_LEN     6   /* an address needs six bytes */

struct proxy_identity {
	/* Static-random address bytes, LSB first: main.c copies them into
	 * bt_addr_le_t.a.val and pairs them with BT_ADDR_LE_RANDOM. Only
	 * meaningful when addr_valid. */
	uint8_t addr[PROXY_ADDR_LEN];
	/* Advertised name ("nrfProxy-3F7A", or the base name alone when there is
	 * no hardware ID). Always NUL-terminated, and zero-padded to the full
	 * buffer so the derivation is byte-deterministic. */
	char name[PROXY_DEVICE_NAME_MAX];
	/* Per-device id for the manufacturer AD field. Left untouched when
	 * there is no hardware ID to derive it from. */
	uint8_t mfg_id[PROXY_MFG_ID_LEN];
	bool addr_valid;
};

/*
 * Derive a unit's identity from the SoC's unique hardware ID.
 *
 * Deterministic by design: the address is recomputed identically on every boot
 * rather than stored, which is what makes it stable without flash — and what
 * makes reboot-stability a property a unit test can check. Without it Zephyr
 * would generate a fresh random-static address from the RNG each boot and every
 * unit would advertise the same name.
 *
 * `hwid_len` is what hwinfo_get_device_id() returned, so it may be negative;
 * anything shorter than PROXY_HWID_MIN_LEN yields the fallback: `base_name`
 * alone, addr_valid = false, and mfg_id untouched.
 */
void proxy_identity_derive(const char *base_name, const uint8_t *hwid,
			   int hwid_len, struct proxy_identity *out);

/* --- Link / advertising policy -------------------------------------------- */

/* A snapshot of the state main.c guards with conn_mutex. Callers fill this in
 * under the lock and then decide outside it. */
struct proxy_link_state {
	bool connected;
	bool adv_active;
	bool link_secure;
	bool locked_mode;
};

/*
 * Whether advertising may be (re)started. Both halves of this guard were field
 * bugs, so treat it as load-bearing:
 *  - `connected`: with CONFIG_BT_MAX_CONN=1 a connectable start while a
 *    connection object is alive fails -ENOMEM and leaves the device unreachable
 *    until reboot.
 *  - `adv_active`: legacy connectable advertising pre-allocates a connection
 *    object, so bt_le_adv_stop() during the fast->slow switch fires the
 *    recycled callback too; without this flag that would start a second,
 *    competing advertiser (-EALREADY).
 */
bool proxy_should_start_adv(const struct proxy_link_state *state);

/* Whether NUS data may flow. Just Works pairing cannot satisfy BT_NUS_AUTHEN's
 * GATT permissions, so this app-level gate is the actual enforcement of the
 * pairing lock — not defence in depth over an already-closed door. */
bool proxy_may_forward(const struct proxy_link_state *state);

/*
 * How long a fresh link may stay unencrypted before the watchdog drops it.
 * Without the watchdog an attacker could squat unencrypted on the single
 * connection slot and block the owner.
 *
 * The window is per-mode, and the pairing one must stay long. Locked: the
 * bonded phone encrypts automatically within a couple of seconds. Pairing: the
 * timeout has to cover a *human* finding and accepting Android's pairing
 * dialog, and disconnecting while SMP is in flight aborts the procedure, which
 * Android reports as a scary "couldn't pair: incorrect PIN" (observed on
 * hardware with a flat 10 s — pairing only succeeded if the user tapped within
 * 10 s of connecting). The long window costs nothing: with no bond stored there
 * is no owner to protect, and anyone connecting could simply pair.
 */
#define PROXY_SECURITY_WINDOW_LOCKED_MS  10000U
#define PROXY_SECURITY_WINDOW_PAIRING_MS 60000U

uint32_t proxy_security_window_ms(bool locked_mode);

/* --- NUS send policy ------------------------------------------------------ */

#define PROXY_ATT_HEADER_LEN      3   /* notification opcode + handle */
#define PROXY_NUS_CHUNK_FALLBACK 20   /* minimum ATT MTU (23) - the header */

/* Largest notification payload for the negotiated ATT MTU. The MTU is re-read
 * per chunk because the peer can negotiate it up mid-connection; the fallback
 * covers a stack reporting a nonsense MTU. */
uint16_t proxy_nus_chunk_limit(uint16_t att_mtu);

enum proxy_send_verdict {
	PROXY_SEND_CONSUMED,  /* copied into the GATT buffer — drop our copy */
	PROXY_SEND_RETRY,     /* no TX buffers right now — keep the data */
	PROXY_SEND_DROP,      /* disconnected / not subscribed — discard */
};

/* Classify a bt_nus_send() return. The stream is repetitive and loss-tolerant
 * by design, so everything that is not a transient buffer shortage drops rather
 * than back-pressures. */
enum proxy_send_verdict proxy_send_result(int bt_nus_send_err);

#ifdef __cplusplus
}
#endif

#endif /* PROXY_CORE_H_ */
