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
 * Both run in thread context — on_uart_rx in ble_write_thread, on_ble_rx in the
 * Bluetooth RX thread — so real filter/framing logic is fine here. (on_uart_rx
 * used to run in the UART ISR; that was moved deliberately, because an ISR is
 * the wrong place for the work these hooks exist for.)
 *
 * Both see transport **chunks**, not framed application messages: on_ble_rx gets
 * one GATT write; on_uart_rx gets whatever the RX ring held contiguously, capped
 * at one notification's worth — which is *not* the UART's chunking. Nothing
 * preserves message boundaries; add reassembly if the data is message-oriented.
 */
#define PROC_BUF_SIZE 512

/* Serial -> phone: bytes received on UART1, before they go out over BLE. */
size_t on_uart_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);

/* Phone -> serial: bytes received over BLE, before they go out UART1. */
size_t on_ble_rx(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size);

/* --- Device-control command (phone -> device) ----------------------------- */
/*
 * A fixed-width 16-byte framed command the phone sends over NUS to control
 * *this* device (drive the relay GPIO, change the UART baud, …), rather than to
 * be forwarded down the UART. The width is fixed so the format never has to grow
 * again: a new control takes a new opcode and reuses the spare payload bytes.
 *
 *   [0]     start    = PROXY_CMD_START (0x22)
 *   [1]     length   = PROXY_CMD_PACKET_LEN (16) — declared length, must match
 *   [2]     opcode   = PROXY_OP_* (which control)
 *   [3..14] payload  = opcode-specific (unused bytes zero)
 *   [15]    checksum = sum(in[0..14]) & 0xFF   (8-bit additive sum)
 *
 * The BLE link is already encrypted (pairing lock), so the checksum only guards
 * against corruption / accidental framing, not tampering.
 *
 * Opcodes and their payloads:
 *   PROXY_OP_RELAY (0x01): payload[0] = 0 disable / 1 enable the relay pin.
 *   PROXY_OP_BAUD  (0x02): payload[0..3] = new UART baud, uint32 little-endian.
 *                          Only an allow-listed rate is accepted (see
 *                          proxy_baud_supported).
 *
 * On an actionable command main.c performs the action and echoes the *exact*
 * 16 bytes back to the phone as an acknowledgement, so the phone can confirm it
 * took effect.
 *
 * Classification (proxy_cmd_parse → struct proxy_cmd::kind):
 *   PROXY_CMD_NONE     framing did not validate (wrong start/length/checksum) —
 *                      not ours; forward down the UART as ordinary data.
 *   PROXY_CMD_INVALID  framed for us but the opcode is unknown or the payload is
 *                      out of range — consume it (a framed control packet is
 *                      never forwarded to the motor) but take no action and send
 *                      no ack.
 *   PROXY_CMD_RELAY_*  drive the relay pin (disable/enable).
 *   PROXY_CMD_SET_BAUD reconfigure the UART to `baud`.
 */
#define PROXY_CMD_START        0x22
#define PROXY_CMD_PACKET_LEN   16
#define PROXY_CMD_PAYLOAD_OFF  3    /* index of the first payload byte */

enum proxy_cmd_op {
	PROXY_OP_RELAY = 0x01,
	PROXY_OP_BAUD  = 0x02,
};

enum proxy_cmd_kind {
	PROXY_CMD_NONE = 0,     /* not a valid command — forward as UART data */
	PROXY_CMD_INVALID,      /* framed for us but bad opcode/payload — drop */
	PROXY_CMD_RELAY_DISABLE,
	PROXY_CMD_RELAY_ENABLE,
	PROXY_CMD_SET_BAUD,
};

struct proxy_cmd {
	enum proxy_cmd_kind kind;
	uint32_t baud;                      /* set for PROXY_CMD_SET_BAUD only */
	uint8_t ack[PROXY_CMD_PACKET_LEN];  /* echo bytes, actionable cmds only */
	size_t ack_len;                     /* 0 unless there is an ack to send */
};

/*
 * Whether `baud` is a UART rate this firmware will switch to. The nRF UARTE only
 * realises a fixed set of rates; this is the intersection with the ones the
 * e-bike controllers use (1200 for Bafang at the slow end, up to 115200).
 */
bool proxy_baud_supported(uint32_t baud);

/*
 * Classify a phone->device chunk and, for an actionable command, fill `out`
 * (which must be non-NULL) with the action, any parameters, and the ack bytes to
 * echo back. Returns out->kind. See the table above for what each kind means.
 */
enum proxy_cmd_kind proxy_cmd_parse(const uint8_t *in, size_t in_len,
				    struct proxy_cmd *out);

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
 *
 * Flat 60 s for both pairing and locked mode (TODO_ARCHITECTURE Task 2):
 * disconnecting mid-SMP aborts pairing and Android reports "incorrect PIN";
 * locked mode needs the same window when the owner phone forgets the bond and
 * must re-pair (still passes the accept list). Long is safe — the filter
 * accept list is the real gate against strangers.
 */
#define PROXY_SECURITY_WINDOW_MS 60000U

uint32_t proxy_security_window_ms(bool locked_mode);

/* --- NUS send policy ------------------------------------------------------ */

#define PROXY_ATT_HEADER_LEN      3   /* notification opcode + handle */
#define PROXY_NUS_CHUNK_FALLBACK 20   /* minimum ATT MTU (23) - the header */

/* Largest notification payload for the negotiated ATT MTU. The MTU is re-read
 * per chunk because the peer can negotiate it up mid-connection; the fallback
 * covers a stack reporting a nonsense MTU. */
uint16_t proxy_nus_chunk_limit(uint16_t att_mtu);

/*
 * Size of the next notification to send from a hook's output buffer, given how
 * much has gone already. Returns 0 when there is nothing left.
 *
 * This exists because the hook may *grow* its input (up to PROC_BUF_SIZE = 512)
 * while a notification is capped at the ATT MTU (<= 244), so one claimed chunk
 * can need several sends. The ring bytes are already consumed by then -- the
 * data lives in the scratch buffer -- so a retry must resend from that buffer
 * and must never re-claim.
 */
size_t proxy_next_slice(size_t out_len, size_t sent, uint16_t max_send);

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
