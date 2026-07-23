/*
 * nrfProxy — forward incoming UART1 serial data to a phone over BLE.
 *
 * Data path:
 *   UART1 RX (async, double-buffered)
 *     -> ring buffer (decouples UART rate from BLE throughput)
 *       -> ble_write_thread
 *         -> Nordic UART Service notification -> phone
 *
 * Connect with "nRF Connect for Mobile" or "nRF Toolbox", subscribe to the
 * NUS TX characteristic, and the serial bytes stream in.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>

#include <bluetooth/services/nus.h>

#include "security_timeout.h"
#include "uart_rx_retry.h"
#include "drop_stats.h"

LOG_MODULE_REGISTER(nrf_proxy, LOG_LEVEL_INF);

/* --- UART (incoming serial) ---------------------------------------------- */

#define UART_NODE         DT_NODELABEL(uart1)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);

#define UART_BUF_SIZE     64        /* size of each async RX buffer */
#define UART_RX_TIMEOUT_US 50000U   /* flush partial data after 50 ms idle */

/* Pool of RX buffers handed to the async UART driver, and a ring buffer that
 * the BLE thread drains. The ring buffer is single-producer (UART ISR) /
 * single-consumer (ble_write_thread), which is safe without extra locking.
 *
 * Sizing: 4 slab buffers because the driver holds up to two at a time
 * (double-buffering) and the release of a finished buffer can lag its
 * replacement request — 2 would deadlock the RX path on that lag, 4 gives
 * margin. The 2048-byte ring is ~180 ms of a sustained 115200 stream: enough
 * to ride out a BLE buffer shortage without dropping, small enough that stale
 * data can't build up a noticeable forwarding delay.
 *
 * The semaphore is binary (max 1) on purpose: it only means "there may be
 * data", and the consumer drains the ring completely per wakeup, so counting
 * every ISR chunk would add nothing. */
K_MEM_SLAB_DEFINE(uart_rx_slab, UART_BUF_SIZE, 4, 4);
RING_BUF_DECLARE(uart_rx_ringbuf, 2048);
K_SEM_DEFINE(rx_data_ready, 0, 1);

/* TX path: bytes received from the phone (NUS write) are queued here and fed
 * out the UART. uart_tx() can only have one transfer in flight, so we stage a
 * contiguous chunk in uart_tx_buf and start the next chunk on UART_TX_DONE. */
RING_BUF_DECLARE(uart_tx_ringbuf, 2048);
static uint8_t uart_tx_buf[UART_BUF_SIZE];
static bool uart_tx_in_progress;
static size_t uart_tx_len; /* bytes of the in-flight uart_tx() transfer */

/* ISR-safe drop counters — reported periodically from ble_write_thread. */
static atomic_t drops_uart_rx_ring;
static atomic_t drops_uart_tx;
static atomic_t last_uart_tx_err; /* latched; 0 means none / cleared after report */

#define DROP_REPORT_INTERVAL_MS 10000U

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
 */
#define PROC_BUF_SIZE 512

/* Serial -> phone: bytes received on UART1, before they go out over BLE. */
static size_t on_uart_rx(const uint8_t *in, size_t in_len,
			 uint8_t *out, size_t out_size)
{
	size_t n = MIN(in_len, out_size);
	

	memcpy(out, in, n);
	return n;
}

/* Phone -> serial: bytes received over BLE, before they go out UART1. */
static size_t on_ble_rx(const uint8_t *in, size_t in_len,
			uint8_t *out, size_t out_size)
{
	size_t n = MIN(in_len, out_size);

	memcpy(out, in, n);
	return n;
}

/* --- Status LEDs --------------------------------------------------------- */
/*
 * One LED per state, driven mutually exclusively (enabling one turns the others
 * off). The physical LED for each role is chosen per-board via the
 * led-connected / led-advertising / led-error aliases in the board overlay, so
 * each board can pick appropriate LEDs/colours (e.g. the XIAO uses green/blue/
 * red). _GET_OR keeps it building if a board omits a role (that LED is then a
 * no-op). Active-low polarity is taken from devicetree, so "on" is logical.
 */
enum status_state {
	STATUS_CONNECTED,
	STATUS_ADVERTISING,
	STATUS_ERROR,
};

static const struct gpio_dt_spec status_leds[] = {
	[STATUS_CONNECTED]   = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led_connected), gpios, {0}),
	[STATUS_ADVERTISING] = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led_advertising), gpios, {0}),
	[STATUS_ERROR]       = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led_error), gpios, {0}),
};

static enum status_state current_status;

/* The advertising LED blinks at a low duty cycle rather than sitting solid-on:
 * a continuously-lit LED draws milliamps and would dominate the battery budget
 * next to sub-100uA advertising. Connected/error states stay solid. The gap
 * between blinks tracks the advertising phase — brisk during fast advertising,
 * lazy once we drop to the slow interval — so the fast->slow switch is visible
 * on the LED even when the (USB CDC-ACM) console isn't. */
#define ADV_BLINK_ON        K_MSEC(30)
#define ADV_BLINK_OFF_FAST  K_MSEC(300)
#define ADV_BLINK_OFF_SLOW  K_MSEC(2000)

static struct k_work_delayable adv_blink_work;
static bool adv_slow_phase;  /* false = fast advertising, true = slow */
static bool adv_blink_on;    /* phase of the advertising blink (file-scope so
			      * set_status_leds can reset it on re-entry) */

static void adv_blink_handler(struct k_work *work)
{
	const struct gpio_dt_spec *led = &status_leds[STATUS_ADVERTISING];

	/* Stop if we've left the advertising state, or this board has no LED. */
	if (current_status != STATUS_ADVERTISING || led->port == NULL) {
		return;
	}

	adv_blink_on = !adv_blink_on;
	gpio_pin_set_dt(led, adv_blink_on);
	k_work_reschedule(&adv_blink_work,
			  adv_blink_on ? ADV_BLINK_ON :
			       (adv_slow_phase ? ADV_BLINK_OFF_SLOW : ADV_BLINK_OFF_FAST));
}

static void leds_init(void)
{
	k_work_init_delayable(&adv_blink_work, adv_blink_handler);

	for (size_t i = 0; i < ARRAY_SIZE(status_leds); i++) {
		const struct gpio_dt_spec *led = &status_leds[i];

		if (led->port == NULL) {
			continue;  /* alias not defined on this board */
		}
		if (!gpio_is_ready_dt(led)) {
			LOG_ERR("status LED %u not ready", (unsigned int)i);
			continue;
		}
		gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	}
}

/* Drive the LEDs for `state`: connected/error are solid, advertising blinks at
 * a low duty cycle (see adv_blink_handler). Turns the other roles off. */
static void set_status_leds(enum status_state state)
{
	current_status = state;

	/* All off first, then drive the active role. */
	for (size_t i = 0; i < ARRAY_SIZE(status_leds); i++) {
		const struct gpio_dt_spec *led = &status_leds[i];

		if (led->port != NULL) {
			gpio_pin_set_dt(led, 0);
		}
	}

	if (state == STATUS_ADVERTISING) {
		/* Start from an ON pulse so re-entering advertising never inherits
		 * a leftover off-gap from the previous session. */
		adv_blink_on = false;
		k_work_reschedule(&adv_blink_work, K_NO_WAIT);
	} else {
		const struct gpio_dt_spec *led = &status_leds[state];

		k_work_cancel_delayable(&adv_blink_work);
		if (led->port != NULL) {
			gpio_pin_set_dt(led, 1);
		}
	}
}

/* --- Bond-reset button --------------------------------------------------- */
/*
 * Optional per-board button that, when held through boot for ~BOND_RESET_HOLD_MS,
 * wipes the stored pairing so a new phone can take ownership (see the pairing
 * lock below). The pin is chosen per-board via the bond-reset alias in the
 * board overlay; _GET_OR falls back to a null spec so a board that omits it
 * simply has no reset button. Hold-at-boot (rather than a runtime long-press)
 * keeps this to a tiny polling loop with no ISR/debounce machinery — the unit
 * is battery-powered and gets power-cycled anyway.
 */
#define BOND_RESET_HOLD_MS 3000

static const struct gpio_dt_spec bond_reset_btn =
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(bond_reset), gpios, {0});

/* Returns true if the bond-reset button was held for the full window at boot.
 * Blinks the advertising LED as "keep holding" feedback; releasing early aborts.
 * Called before bt_enable(); the actual bt_unpair() happens after settings_load
 * (there's nothing stored to erase until then). */
static bool bond_reset_requested(void)
{
	const struct gpio_dt_spec *led = &status_leds[STATUS_ADVERTISING];

	if (bond_reset_btn.port == NULL || !gpio_is_ready_dt(&bond_reset_btn)) {
		return false;   /* board defines no reset button */
	}

	gpio_pin_configure_dt(&bond_reset_btn, GPIO_INPUT);

	if (!gpio_pin_get_dt(&bond_reset_btn)) {
		return false;   /* not pressed at boot */
	}

	LOG_INF("Bond-reset button down; hold %d ms to wipe pairing",
		BOND_RESET_HOLD_MS);

	for (int elapsed = 0; elapsed < BOND_RESET_HOLD_MS; elapsed += 100) {
		if (!gpio_pin_get_dt(&bond_reset_btn)) {
			LOG_INF("Bond-reset aborted (released early)");
			if (led->port != NULL) {
				gpio_pin_set_dt(led, 0);
			}
			return false;
		}
		if (led->port != NULL) {
			gpio_pin_set_dt(led, (elapsed / 100) & 1);
		}
		k_msleep(100);
	}

	if (led->port != NULL) {
		gpio_pin_set_dt(led, 0);
	}
	LOG_INF("Bond-reset confirmed; pairing will be wiped");
	return true;
}

/* --- BLE ----------------------------------------------------------------- */

#define DEVICE_NAME       CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN   (sizeof(DEVICE_NAME) - 1)

/* Runtime advertised name: CONFIG_BT_DEVICE_NAME plus a "-XXXX" suffix derived
 * from the chip's hardware ID (filled in by identity_init()). */
static char device_name[DEVICE_NAME_LEN + sizeof("-XXXX")];

/* Manufacturer-specific advertising data, so a scanner can filter for *our*
 * boards without connecting. Layout on the wire:
 *   [company_id lo][company_id hi][magic 'N'][magic 'P'][per-device id x4]
 * The company id is little-endian; 0xFFFF is the SIG "for testing" id (replace
 * with an assigned one before shipping). 'N','P' is a fixed tag an Android
 * ScanFilter matches on; the last 4 bytes are set per-device in identity_init().
 */
#define COMPANY_ID_LO 0xFF
#define COMPANY_ID_HI 0xFF

static uint8_t mfg_data[] = {
	COMPANY_ID_LO, COMPANY_ID_HI,  /* company id, little-endian */
	'N', 'P',                      /* magic tag — what a scanner filters on */
	0, 0, 0, 0,                    /* per-device id, set in identity_init() */
};

static struct bt_conn *current_conn;
/* Guards current_conn so the writer thread can take a reference before use,
 * preventing a use-after-free if the peer disconnects mid-send. Also guards
 * link_secure and adv_active below (same discipline). */
K_MUTEX_DEFINE(conn_mutex);

/* True once the current link has reached security level 2 (encrypted). NUS
 * data flows only while this is set — this app-level gate is the actual
 * enforcement, since Just Works pairing can't satisfy BT_NUS_AUTHEN's GATT
 * permissions (see PAIRING_PLAN.md / prj.conf). Guarded by conn_mutex. */
static bool link_secure;

/* True when a bond is stored: advertise with the filter accept list so only
 * the bonded phone can connect. Cleared in pairing mode (no bond) and after a
 * factory reset. Read when (re)starting advertising to pick filtered params.
 *
 * Mutex exemption: read WITHOUT conn_mutex in on_connected and
 * adv_params_fast/slow(). Safe — single-writer bool; writes happen at boot
 * (pre-connection) or from pairing_complete while already connected. */ 
static bool locked_mode;

/* Not const: ad[1].data_len is set once at runtime from the resolved name. */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, device_name, 0),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/* Give each unit a stable, distinguishable identity derived from the SoC's
 * unique hardware ID:
 *   - a fixed random-static BT address (constant across reboots, unlike the
 *     default which is regenerated from the RNG each boot), and
 *   - a "-XXXX" name suffix so units are tellable apart in a scanner.
 * bt_id_create() must run before bt_enable(); the name is applied after.
 * If the hardware ID can't be read we fall back to the compile-time defaults. */
static void identity_init(void)
{
	uint8_t hwid[8];
	ssize_t len;

	/* Default name regardless of what follows; suffix appended on success. */
	memcpy(device_name, DEVICE_NAME, DEVICE_NAME_LEN + 1);

	len = hwinfo_get_device_id(hwid, sizeof(hwid));
	if (len < 6) {
		LOG_WRN("No hardware ID (%d); using default name/address",
			(int)len);
		return;
	}

	/* Fixed static-random address from the low 6 bytes of the chip id.
	 * BT_ADDR_SET_STATIC forces the two MSBs that mark it static-random. */
	bt_addr_le_t addr = { .type = BT_ADDR_LE_RANDOM };

	memcpy(addr.a.val, hwid, 6);
	BT_ADDR_SET_STATIC(&addr.a);

	int err = bt_id_create(&addr, NULL);
	if (err < 0) {
		LOG_WRN("bt_id_create failed (%d); using random address", err);
	}

	/* Unique name suffix from the top id bytes (e.g. "nrfProxy-3F7A"). */
	snprintf(device_name + DEVICE_NAME_LEN, sizeof("-XXXX"), "-%02X%02X",
		 hwid[5], hwid[4]);

	/* Per-device id in the manufacturer advertising data (after the tag). */
	memcpy(&mfg_data[4], hwid, 4);
}

/* Two-phase advertising for power: advertise at the fast interval for quick
 * discovery/connection right after boot or a disconnect, then drop to the slow
 * (~1 s) interval to cut standby radio current. adv_slow_work makes the switch
 * after ADV_FAST_DURATION. */
#define ADV_FAST_DURATION K_SECONDS(30)

/* Fast/slow advertising params, each in an open and a filtered variant. In
 * locked mode BT_LE_ADV_OPT_FILTER_CONN makes the controller ignore connection
 * requests from anyone not on the filter accept list (the bonded phone). We
 * deliberately do NOT add BT_LE_ADV_OPT_FILTER_SCAN_REQ, so non-owner phones
 * (and nRF Connect) can still *see* the device in scans — they just can't
 * connect. advertising_start() / adv_slow_handler() pick the variant by
 * locked_mode via adv_params_fast()/adv_params_slow(). The open fast params
 * match the old BT_LE_ADV_CONN_FAST_2 (100–150 ms). */
static const struct bt_le_adv_param adv_param_fast = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN,
	BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

static const struct bt_le_adv_param adv_param_fast_filtered = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_FILTER_CONN,
	BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

static const struct bt_le_adv_param adv_param_slow = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN,
	BT_GAP_ADV_SLOW_INT_MIN,   /* 1.0 s */
	BT_GAP_ADV_SLOW_INT_MAX,   /* 1.2 s */
	NULL);

static const struct bt_le_adv_param adv_param_slow_filtered = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_FILTER_CONN,
	BT_GAP_ADV_SLOW_INT_MIN, BT_GAP_ADV_SLOW_INT_MAX, NULL);

static const struct bt_le_adv_param *adv_params_fast(void)
{
	return locked_mode ? &adv_param_fast_filtered : &adv_param_fast;
}

static const struct bt_le_adv_param *adv_params_slow(void)
{
	return locked_mode ? &adv_param_slow_filtered : &adv_param_slow;
}

static struct k_work_delayable adv_slow_work;

/* Retry timer for failed advertising starts. A failure is typically a
 * transient resource shortage (e.g. no free connection object at that
 * instant); without a retry the device would sit in the error state,
 * unreachable until a power cycle — unacceptable for an unattended bridge.
 * The error LED still shows during the retry window. One second is fast
 * enough to feel seamless, slow enough not to hammer a persistent fault. */
#define ADV_RETRY_DELAY K_SECONDS(1)
static struct k_work_delayable adv_retry_work;

/* UART async RX can stop (slab empty at BUF_REQUEST → UART_RX_DISABLED). The
 * DISABLED handler retries inline; if that fails, this work item keeps trying
 * from thread context until enable succeeds. Mirror of adv_retry_work. */
#define UART_RX_RETRY_DELAY K_MSEC(100)
static struct k_work_delayable uart_rx_retry_work;
static bool uart_rx_retry_warned;

static void uart_rx_retry_handler(struct k_work *work)
{
	uint8_t *buf = NULL;
	int err;

	ARG_UNUSED(work);

	if (k_mem_slab_alloc(&uart_rx_slab, (void **)&buf, K_NO_WAIT) != 0) {
		if (uart_rx_retry_warn_once(&uart_rx_retry_warned)) {
			LOG_WRN("UART RX restart: no slab buffer, retrying");
		}
		k_work_reschedule(&uart_rx_retry_work, UART_RX_RETRY_DELAY);
		return;
	}

	err = uart_rx_enable(uart_dev, buf, UART_BUF_SIZE, UART_RX_TIMEOUT_US);
	if (uart_rx_enable_succeeded(err)) {
		/* -EBUSY: ISR or a prior attempt already re-enabled — drop the
		 * spare buffer we allocated; the live path owns its own. */
		if (err == -EBUSY) {
			k_mem_slab_free(&uart_rx_slab, buf);
		}
		uart_rx_retry_clear_latch(&uart_rx_retry_warned);
		return;
	}

	k_mem_slab_free(&uart_rx_slab, buf);
	if (uart_rx_retry_warn_once(&uart_rx_retry_warned)) {
		LOG_WRN("UART RX restart failed (err %d), retrying", err);
	}
	k_work_reschedule(&uart_rx_retry_work, UART_RX_RETRY_DELAY);
}

/* Whether an advertiser is currently live. Guarded by conn_mutex (like
 * current_conn). Needed because legacy connectable advertising pre-allocates a
 * connection object, so bt_le_adv_stop() — e.g. during the fast->slow switch —
 * frees that object and fires the recycled callback; without this flag
 * on_recycled would start a second, competing advertiser (-EALREADY). */
static bool adv_active;

static void advertising_start(void)
{
	int err;

	k_mutex_lock(&conn_mutex, K_FOREVER);
	if (current_conn || adv_active) {
		/* Connected, or an advertiser is already running (recycled
		 * fired for the object released by the fast->slow switch). */
		k_mutex_unlock(&conn_mutex);
		return;
	}
	err = bt_le_adv_start(adv_params_fast(), ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	adv_active = (err == 0);
	k_mutex_unlock(&conn_mutex);

	if (err) {
		LOG_ERR("Advertising failed to start (err %d), retrying", err);
		set_status_leds(STATUS_ERROR);
		k_work_reschedule(&adv_retry_work, ADV_RETRY_DELAY);
		return;
	}
	LOG_INF("Advertising as \"%s\" (fast, %s)", device_name,
		locked_mode ? "locked" : "pairing");
	adv_slow_phase = false;   /* brisk LED blink during the fast phase */
	set_status_leds(STATUS_ADVERTISING);
	k_work_reschedule(&adv_slow_work, ADV_FAST_DURATION);
}

/* A stale retry firing after recovery (or after a connection arrived) is a
 * no-op via the current_conn/adv_active guard in advertising_start(), so the
 * pending work item never needs cancelling. */
static void adv_retry_handler(struct k_work *work)
{
	advertising_start();
}

/* Restart advertising at the slow interval. Skipped if a connection arrived in
 * the meantime (on_connected also cancels this work, but guard the race). */
static void adv_slow_handler(struct k_work *work)
{
	int err;

	k_mutex_lock(&conn_mutex, K_FOREVER);
	if (current_conn || !adv_active) {
		k_mutex_unlock(&conn_mutex);
		return;
	}

	err = bt_le_adv_stop();
	if (err) {
		LOG_WRN("adv stop failed (%d)", err);
		k_mutex_unlock(&conn_mutex);
		/* Still advertising fast (reachable) — just retry the switch. */
		k_work_reschedule(&adv_slow_work, ADV_RETRY_DELAY);
		return;
	}
	/* adv_active stays true across the stop->start gap: the stop above
	 * frees the advertiser's pre-allocated connection object and fires the
	 * recycled callback, which must not start a competing advertiser. */
	err = bt_le_adv_start(adv_params_slow(), ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		adv_active = false;
	}
	k_mutex_unlock(&conn_mutex);

	if (err) {
		/* Distinguish the benign connect-vs-timer race from a genuine
		 * failure. This handler runs on the system workqueue; a central can
		 * connect in the gap between the guard check above and bt_le_adv_start,
		 * consuming the single connection object so the slow start fails with
		 * -ENOMEM before on_connected (BT RX thread) has updated state. */
		struct bt_conn *conn;

		k_mutex_lock(&conn_mutex, K_FOREVER);
		conn = current_conn;
		k_mutex_unlock(&conn_mutex);

		if (conn) {
			/* Connected after all: on_connected owns the LED/adv state,
			 * so this failure is expected — don't flash the error LED. */
			LOG_INF("slow adv aborted: connected during switch");
			return;
		}

		/* No connection visible yet. Either the same race a hair earlier
		 * (on_connected is about to run and the retry will no-op) or a real
		 * failure. Recover via the retry path — but don't light the error LED
		 * here: advertising_start() sets STATUS_ERROR only if the retry
		 * genuinely can't advertise, so a transient race never shows red. */
		LOG_WRN("slow advertising didn't start (err %d), retrying", err);
		k_work_reschedule(&adv_retry_work, ADV_RETRY_DELAY);
		return;
	}
	LOG_INF("Advertising (slow)");
	adv_slow_phase = true;   /* LED drops to the lazy blink */
}

/* --- Pairing lock: bonds, accept list, encryption enforcement ------------ */

static void bond_count_cb(const struct bt_bond_info *info, void *user_data)
{
	size_t *count = user_data;

	(*count)++;
}

static void bond_accept_list_cb(const struct bt_bond_info *info, void *user_data)
{
	int err = bt_le_filter_accept_list_add(&info->addr);

	if (err) {
		LOG_WRN("accept list add failed (%d)", err);
	}
}

/* Recompute locked_mode from the stored bonds and rebuild the filter accept
 * list to match. bt_bond_info carries the peer *identity* address, which is
 * what the accept list needs (the controller resolving list, populated by the
 * stack from the bonded IRK, then matches the phone's rotating RPA). Safe to
 * call only when no advertiser is running (boot, or while connected) — the
 * accept list can't be modified in use. */
static void refresh_locked_mode(void)
{
	size_t bonds = 0;

	bt_foreach_bond(BT_ID_DEFAULT, bond_count_cb, &bonds);

	bt_le_filter_accept_list_clear();
	if (bonds > 0) {
		bt_foreach_bond(BT_ID_DEFAULT, bond_accept_list_cb, NULL);
	}

	locked_mode = (bonds > 0);
	LOG_INF("%s mode (%u bond%s)", locked_mode ? "Locked" : "Pairing",
		(unsigned int)bonds, bonds == 1 ? "" : "s");
}

/* If a fresh link never encrypts, drop it. Armed on connect, cancelled once
 * security_changed reports level 2.
 *
 * Why 60 s (and why not shorter in locked mode):
 * (a) Disconnecting while SMP is in flight aborts pairing; Android then reports
 *     a scary "couldn't pair: incorrect PIN" failure (observed on hardware with
 *     a flat 10 s window). The window must cover a human finding and accepting
 *     the pairing dialog.
 * (b) Locked mode can need that dialog too: if the owner phone "forgets" the
 *     device in Android Bluetooth settings it still passes the filter accept
 *     list (same identity/IRK) and must re-pair into the single bond slot —
 *     the documented no-factory-reset recovery path. A 10 s locked window
 *     killed that mid-dialog.
 * (c) Long is safe: with BT_LE_ADV_OPT_FILTER_CONN the accept list is the real
 *     gate — strangers never get a connection object. The watchdog only bounds
 *     a misbehaving *authorized* peer (or one that already holds the owner's
 *     IRK). Collapsing pairing/locked into one window drops dead state.
 */
#define SECURITY_TIMEOUT K_MSEC(SECURITY_TIMEOUT_MS)
static struct k_work_delayable security_timeout_work;

/* Pairing is driven by the CENTRAL, not the peripheral: we deliberately do NOT
 * send an SMP Security Request. A peripheral-initiated Security Request makes
 * Android raise a premature pre-negotiation pairing consent AND then the real
 * LE-Secure-Connections consent — two dialogs for one bond, first appears to
 * fail (confirmed on a Pixel 6a via the BluetoothBondStateMachine trace: one
 * BONDING→BONDED session issuing two ACTION_PAIRING_REQUEST intents,
 * pairingAlgo 0 then 3). Instead the bafflingvision app calls createBond()
 * after service discovery, so Android owns a single, clean pairing flow; on a
 * bonded reconnect the central initiates LTK encryption on its own. Either way
 * on_security_changed reports level 2 and the watchdog below is cancelled.
 * (Trade-off: a non-app central like nRF Connect must initiate pairing itself;
 * if nothing encrypts the link within the window, the watchdog drops it.) */

/* If the current link is still up and not yet encrypted, take a reference to
 * it (so a concurrent disconnect can't free it under us — same discipline as
 * ble_write_thread) and return it; the caller must bt_conn_unref(). Returns
 * NULL when there's nothing to act on. */
static struct bt_conn *ref_unsecured_conn(void)
{
	struct bt_conn *conn = NULL;

	k_mutex_lock(&conn_mutex, K_FOREVER);
	if (current_conn && !link_secure) {
		conn = bt_conn_ref(current_conn);
	}
	k_mutex_unlock(&conn_mutex);

	return conn;
}

static void security_timeout_handler(struct k_work *work)
{
	struct bt_conn *conn = ref_unsecured_conn();

	if (conn) {
		LOG_WRN("Link not encrypted within timeout; disconnecting");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
	}
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	if (err) {
		LOG_WRN("Security change failed (level %d, err %d); watchdog "
			"will disconnect", level, err);
		return;
	}

	LOG_INF("Security level %d", level);

	if (level >= BT_SECURITY_L2) {
		k_mutex_lock(&conn_mutex, K_FOREVER);
		link_secure = true;
		k_mutex_unlock(&conn_mutex);
		k_work_cancel_delayable(&security_timeout_work);
		/* Data was gated until now — wake the writer to flush. */
		k_sem_give(&rx_data_ready);
	}
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Paired %s (bonded=%d)", addr, bonded);

	if (!bonded) {
		return;
	}

	/* First owner just bonded: rebuild the accept list from the stored bond
	 * and enter locked mode. The advertiser is stopped while connected, so
	 * modifying the accept list here is safe; the recycled->advertising_start
	 * path brings the advertiser back up filtered after this phone leaves. */
	refresh_locked_mode();
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("Pairing failed (reason %d); watchdog will disconnect", reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	/* Either way the connection attempt consumed the advertiser. */
	k_mutex_lock(&conn_mutex, K_FOREVER);
	adv_active = false;
	if (!err) {
		current_conn = bt_conn_ref(conn);
		link_secure = false;   /* no data until the link encrypts */
	}
	k_mutex_unlock(&conn_mutex);

	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		/* The recycled callback restarts advertising once the failed
		 * connection's object is returned to the pool. */
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected: %s", addr);

	/* Advertising stopped on connect; don't let the fast->slow switch fire. */
	k_work_cancel_delayable(&adv_slow_work);

	/* We do NOT request security here (see the security_timeout_work comment):
	 * the central drives pairing/encryption. Just arm the watchdog so a link
	 * that never encrypts gets dropped. */
	k_work_reschedule(&security_timeout_work, SECURITY_TIMEOUT);

	set_status_leds(STATUS_CONNECTED);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason %u)", reason);

	k_work_cancel_delayable(&security_timeout_work);

	k_mutex_lock(&conn_mutex, K_FOREVER);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	link_secure = false;
	k_mutex_unlock(&conn_mutex);

	/* Drop anything buffered so the next client starts on a clean stream.
	 * Don't touch the ring buffer from here: it is single-producer (UART
	 * ISR) / single-consumer (ble_write_thread), and a ring_buf_reset()
	 * from this (BT host) thread would race a concurrent ISR put and could
	 * corrupt the indices. Instead wake the writer thread — it sees
	 * current_conn == NULL and discards from the consumer side, which is
	 * safe against the producer.
	 *
	 * Deliberately only the RX ring: uart_tx_ringbuf (phone→serial) is left
	 * alone — ≤2048 B drains in ~180 ms at 115200, and resetting it from
	 * this BT thread would violate its SPSC contract the same way. */
	k_sem_give(&rx_data_ready);

	/* Do NOT restart advertising here: at this point the connection object
	 * is still allocated (the stack unrefs it after the callbacks return),
	 * and with CONFIG_BT_MAX_CONN=1 connectable advertising then fails with
	 * -ENOMEM — leaving the device unreachable until reboot. The recycled
	 * callback below fires once the object is back in the pool. */
}

/* A connection object was returned to the pool — for us that means the (only)
 * connection fully went away (clean disconnect or failed connect attempt), so
 * this is the safe point to become connectable again. */
static void on_recycled(void)
{
	advertising_start();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
	.security_changed = on_security_changed,
};

/* Start a UART transmission if one isn't already running. Safe to call from
 * both thread context (NUS write) and ISR context (UART_TX_DONE); the
 * in-progress flag is checked under irq_lock so only one transfer starts. */
static void uart_tx_kick(void)
{
	unsigned int key = irq_lock();

	if (uart_tx_in_progress) {
		irq_unlock(key);
		return;
	}

	uint32_t len = ring_buf_get(&uart_tx_ringbuf, uart_tx_buf,
				    sizeof(uart_tx_buf));
	if (len == 0) {
		irq_unlock(key);
		return;
	}

	uart_tx_in_progress = true;
	uart_tx_len = len;
	irq_unlock(key);

	int tx_err = uart_tx(uart_dev, uart_tx_buf, len, SYS_FOREVER_US);
	if (tx_err != 0) {
		/* The staged chunk is deliberately dropped rather than re-queued:
		 * a failing uart_tx() here means the driver is in a state where
		 * retrying inline would likely fail too, and this stream is
		 * repetitive/loss-tolerant by design. Clearing the flag lets the
		 * next NUS write (or TX_DONE) kick the pipeline back to life. */
		atomic_add(&drops_uart_tx, (atomic_val_t)len);
		atomic_set(&last_uart_tx_err, tx_err);
		uart_tx_in_progress = false;
		uart_tx_len = 0;
	}
}

/* Phone -> device: data written to the NUS RX characteristic goes out UART1. */
static void nus_received(struct bt_conn *conn, const uint8_t *const data,
			 uint16_t len)
{
	/* Drop writes until the link is encrypted (security level 2). Without
	 * BT_NUS_AUTHEN the GATT perms are open, so this app-level gate is the
	 * actual enforcement of the pairing lock. */
	k_mutex_lock(&conn_mutex, K_FOREVER);
	bool secure = link_secure;
	k_mutex_unlock(&conn_mutex);

	if (!secure) {
		LOG_WRN("Dropping %u NUS bytes: link not encrypted", len);
		return;
	}

	/* Runs in the Bluetooth RX thread, one call at a time, so a static
	 * scratch buffer is safe here and keeps it off the stack. */
	static uint8_t proc_buf[PROC_BUF_SIZE];
	size_t out_len = on_ble_rx(data, len, proc_buf, sizeof(proc_buf));

	if (out_len == 0) {
		return;
	}

	uint32_t queued = ring_buf_put(&uart_tx_ringbuf, proc_buf, out_len);

	if (queued < out_len) {
		LOG_WRN("UART TX buffer full, dropped %u bytes",
			(unsigned int)(out_len - queued));
	}

	uart_tx_kick();
}

static void nus_send_enabled(enum bt_nus_send_status status)
{
	LOG_INF("NUS notifications %s",
		status == BT_NUS_SEND_STATUS_ENABLED ? "enabled" : "disabled");
}

static struct bt_nus_cb nus_cb = {
	.received = nus_received,
	.send_enabled = nus_send_enabled,
};

/* --- UART async callback (runs in ISR context) --------------------------- */

static void uart_cb(const struct device *dev, struct uart_event *evt,
		    void *user_data)
{
	uint8_t *buf;
	int err;

	switch (evt->type) {
	case UART_TX_DONE:
		uart_tx_in_progress = false;
		uart_tx_len = 0;
		uart_tx_kick();
		break;

	case UART_TX_ABORTED: {
		/* evt->data.tx.len = bytes actually sent before abort; remainder
		 * was never transmitted but we already consumed it from the ring. */
		size_t sent = evt->data.tx.len;
		size_t aborted = (uart_tx_len > sent) ? (uart_tx_len - sent) : 0;

		if (aborted > 0) {
			atomic_add(&drops_uart_tx, (atomic_val_t)aborted);
		}
		uart_tx_in_progress = false;
		uart_tx_len = 0;
		uart_tx_kick();
		break;
	}

	case UART_RX_RDY: {
		/* Pass the freshly received bytes through the interception hook,
		 * then queue the result for BLE. This runs in ISR context, so
		 * keep on_uart_rx() light. proc_buf is safe as static because the
		 * UART ISR is not re-entrant. */
		static uint8_t proc_buf[PROC_BUF_SIZE];
		size_t out_len = on_uart_rx(evt->data.rx.buf + evt->data.rx.offset,
					    evt->data.rx.len, proc_buf,
					    sizeof(proc_buf));
		if (out_len > 0) {
			uint32_t queued = ring_buf_put(&uart_rx_ringbuf, proc_buf,
						       out_len);
			uint32_t dropped = drop_stats_rx_overflow(out_len, queued);

			if (dropped > 0) {
				atomic_add(&drops_uart_rx_ring,
					   (atomic_val_t)dropped);
			}
			k_sem_give(&rx_data_ready);
		}
		break;
	}

	case UART_RX_BUF_REQUEST:
		/* Provide the next buffer for double-buffered reception. If the
		 * slab is empty we deliberately answer with nothing: the driver
		 * then runs out of buffers and raises UART_RX_DISABLED, whose
		 * handler below restarts reception once a buffer is free again.
		 * That path is the recovery mechanism — don't "fix" the silent
		 * failure here without covering it there. */
		if (k_mem_slab_alloc(&uart_rx_slab, (void **)&buf, K_NO_WAIT) == 0) {
			uart_rx_buf_rsp(dev, buf, UART_BUF_SIZE);
		}
		break;

	case UART_RX_BUF_RELEASED:
		if (evt->data.rx_buf.buf) {
			k_mem_slab_free(&uart_rx_slab, evt->data.rx_buf.buf);
		}
		break;

	case UART_RX_DISABLED:
		/* Reception stopped (e.g. out of buffers): restart it. Keep the
		 * inline attempt; on any failure the delayed work retries from
		 * thread context (slab/enable can fail under ISR pressure). */
		if (k_mem_slab_alloc(&uart_rx_slab, (void **)&buf, K_NO_WAIT) == 0) {
			err = uart_rx_enable(dev, buf, UART_BUF_SIZE,
					     UART_RX_TIMEOUT_US);
			if (uart_rx_enable_succeeded(err)) {
				if (err == -EBUSY) {
					k_mem_slab_free(&uart_rx_slab, buf);
				}
				uart_rx_retry_clear_latch(&uart_rx_retry_warned);
				break;
			}
			k_mem_slab_free(&uart_rx_slab, buf);
		}
		k_work_reschedule(&uart_rx_retry_work, UART_RX_RETRY_DELAY);
		break;

	default:
		break;
	}
}

static int uart_init(void)
{
	uint8_t *buf;
	int err;

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	err = uart_callback_set(uart_dev, uart_cb, NULL);
	if (err) {
		LOG_ERR("uart_callback_set failed (%d)", err);
		return err;
	}

	err = k_mem_slab_alloc(&uart_rx_slab, (void **)&buf, K_NO_WAIT);
	if (err) {
		return err;
	}

	err = uart_rx_enable(uart_dev, buf, UART_BUF_SIZE, UART_RX_TIMEOUT_US);
	if (err) {
		k_mem_slab_free(&uart_rx_slab, buf);
		return err;
	}

	return 0;
}

/* --- BLE writer thread: ring buffer -> NUS notifications ------------------ */

/* Discard everything in the RX ring buffer. Must only be called from
 * ble_write_thread: the ring is single-producer (UART ISR) / single-consumer
 * (this thread), and claim/finish is a pure consumer operation, safe against a
 * concurrent ISR put. ring_buf_reset() is NOT — it rewrites both indices and
 * corrupts the buffer if the ISR produces mid-reset (this was a real latent
 * bug, hit on every disconnect while serial data streamed). */
static void uart_rx_ringbuf_drain(void)
{
	uint8_t *buf;
	uint32_t len;

	while ((len = ring_buf_get_claim(&uart_rx_ringbuf, &buf, UINT32_MAX)) > 0) {
		ring_buf_get_finish(&uart_rx_ringbuf, len);
	}
}

static void ble_write_thread(void)
{
	uint8_t *chunk;
	uint32_t claimed;
	uint16_t max_send;
	int err;
	uint32_t last_rx = 0;
	uint32_t last_tx = 0;
	uint32_t last_report_ms = 0;

	for (;;) {
		k_sem_take(&rx_data_ready, K_FOREVER);

		while (!ring_buf_is_empty(&uart_rx_ringbuf)) {
			struct bt_conn *conn = NULL;

			/* Take our own reference so the connection object can't be
			 * freed by a concurrent disconnect while we use it. Only
			 * forward once the link is encrypted (pairing lock). */
			k_mutex_lock(&conn_mutex, K_FOREVER);
			if (current_conn && link_secure) {
				conn = bt_conn_ref(current_conn);
			}
			k_mutex_unlock(&conn_mutex);

			if (!conn) {
				/* Nobody listening, or link not yet encrypted:
				 * discard buffered data. security_changed wakes
				 * us again once the link secures. Intentional —
				 * not counted as a drop (put-side overflow is). */
				uart_rx_ringbuf_drain();
				break;
			}

			/* Notification payload is limited to ATT_MTU - 3 (opcode +
			 * handle overhead). The MTU is re-read every chunk because
			 * the peer can negotiate it up mid-connection. The 20-byte
			 * fallback is the minimum ATT MTU (23) minus that overhead
			 * — used only if the stack reports a nonsense MTU. */
			max_send = bt_gatt_get_mtu(conn);
			max_send = (max_send > 3) ? (max_send - 3) : 20;

			claimed = ring_buf_get_claim(&uart_rx_ringbuf, &chunk,
						     max_send);
			if (claimed == 0) {
				bt_conn_unref(conn);
				break;
			}

			err = bt_nus_send(conn, chunk, claimed);
			if (err == 0) {
				/* Data was copied into the GATT buffer. */
				ring_buf_get_finish(&uart_rx_ringbuf, claimed);
			} else if (err == -ENOMEM || err == -EAGAIN) {
				/* No TX buffers right now: keep data, retry. */
				ring_buf_get_finish(&uart_rx_ringbuf, 0);
				k_sleep(K_MSEC(10));
			} else {
				/* Disconnected / not subscribed: drop chunk. */
				ring_buf_get_finish(&uart_rx_ringbuf, claimed);
			}

			bt_conn_unref(conn);
		}

		/* Periodic drop report (thread context only — never from ISR). */
		{
			uint32_t rx = (uint32_t)atomic_get(&drops_uart_rx_ring);
			uint32_t tx = (uint32_t)atomic_get(&drops_uart_tx);
			bool changed = (rx != last_rx) || (tx != last_tx);
			uint32_t now = k_uptime_get_32();

			if (drop_stats_should_report(now, last_report_ms,
						     DROP_REPORT_INTERVAL_MS,
						     changed)) {
				int tx_err = (int)atomic_get(&last_uart_tx_err);

				LOG_WRN("drops: uart_rx_ring=%u uart_tx=%u "
					"last_uart_tx_err=%d",
					rx, tx, tx_err);
				last_rx = rx;
				last_tx = tx;
				last_report_ms = now;
			}
		}
	}
}

/* Preemptible priority 7: below the BT host/controller threads (cooperative,
 * negative priorities), so pushing proxy data can never starve the stack that
 * has to deliver it — but above the idle-level housekeeping threads. */
K_THREAD_DEFINE(ble_write_thread_id, 2048, ble_write_thread, NULL, NULL, NULL,
		7, 0, 0);

/* --- main ---------------------------------------------------------------- */

int main(void)
{
	int err;

	LOG_INF("nrfProxy: UART1 -> BLE NUS bridge starting");

	leds_init();
	k_work_init_delayable(&adv_slow_work, adv_slow_handler);
	k_work_init_delayable(&adv_retry_work, adv_retry_handler);
	k_work_init_delayable(&uart_rx_retry_work, uart_rx_retry_handler);
	k_work_init_delayable(&security_timeout_work, security_timeout_handler);

	/* Check the bond-reset button before bringing up BLE: held through boot,
	 * it wipes the stored pairing (applied after settings_load below). */
	bool factory_reset = bond_reset_requested();

	err = uart_init();
	if (err) {
		LOG_ERR("UART init failed (%d)", err);
		set_status_leds(STATUS_ERROR);
		return 0;
	}

	/* Resolve the per-device address (must precede bt_enable) and name. */
	identity_init();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		set_status_leds(STATUS_ERROR);
		return 0;
	}
	LOG_INF("Bluetooth initialized");

	/* Log pairing outcomes and flip into locked mode after the first bond. */
	bt_conn_auth_info_cb_register(&auth_info_cb);

	/* Restore bonds from flash (required with CONFIG_BT_SETTINGS, and must
	 * run before advertising_start so locked_mode is known). With
	 * CONFIG_BT_DEVICE_NAME_DYNAMIC this can also restore a stored GAP
	 * name — so bt_set_name runs after this, not before. */
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load();
	}

	/* Apply the resolved name to the GAP Device Name characteristic and the
	 * advertising payload (after settings_load so a restored name cannot
	 * overwrite the hardware-ID-derived one). */
	err = bt_set_name(device_name);
	if (err) {
		LOG_WRN("bt_set_name failed (%d)", err);
	}
	ad[1].data_len = strlen(device_name);

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("bt_nus_init failed (%d)", err);
		set_status_leds(STATUS_ERROR);
		return 0;
	}

	/* Apply a boot-time factory reset now that bonds are loaded (there was
	 * nothing to erase before settings_load). */
	if (factory_reset) {
		err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
		LOG_INF("Bond-reset: wiped pairing (err %d); pairing mode", err);
	}

	/* Decide open vs. filtered advertising from the surviving bonds and build
	 * the accept list to match. */
	refresh_locked_mode();

	advertising_start();

	/* main() returning is normal here: the application lives on in the BLE
	 * callbacks, the UART ISR, and ble_write_thread. */
	return 0;
}
