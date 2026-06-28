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
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <bluetooth/services/nus.h>

LOG_MODULE_REGISTER(nrf_proxy, LOG_LEVEL_INF);

/* --- UART (incoming serial) ---------------------------------------------- */

#define UART_NODE         DT_NODELABEL(uart1)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);

#define UART_BUF_SIZE     64        /* size of each async RX buffer */
#define UART_RX_TIMEOUT_US 50000U   /* flush partial data after 50 ms idle */

/* Pool of RX buffers handed to the async UART driver, and a ring buffer that
 * the BLE thread drains. The ring buffer is single-producer (UART ISR) /
 * single-consumer (ble_write_thread), which is safe without extra locking. */
K_MEM_SLAB_DEFINE(uart_rx_slab, UART_BUF_SIZE, 4, 4);
RING_BUF_DECLARE(uart_rx_ringbuf, 2048);
K_SEM_DEFINE(rx_data_ready, 0, 1);

/* TX path: bytes received from the phone (NUS write) are queued here and fed
 * out the UART. uart_tx() can only have one transfer in flight, so we stage a
 * contiguous chunk in uart_tx_buf and start the next chunk on UART_TX_DONE. */
RING_BUF_DECLARE(uart_tx_ringbuf, 2048);
static uint8_t uart_tx_buf[UART_BUF_SIZE];
static bool uart_tx_in_progress;

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

static void adv_blink_handler(struct k_work *work)
{
	const struct gpio_dt_spec *led = &status_leds[STATUS_ADVERTISING];
	static bool on;

	/* Stop if we've left the advertising state, or this board has no LED. */
	if (current_status != STATUS_ADVERTISING || led->port == NULL) {
		return;
	}

	on = !on;
	gpio_pin_set_dt(led, on);
	k_work_reschedule(&adv_blink_work,
			  on ? ADV_BLINK_ON :
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
		k_work_reschedule(&adv_blink_work, K_NO_WAIT);
	} else {
		const struct gpio_dt_spec *led = &status_leds[state];

		k_work_cancel_delayable(&adv_blink_work);
		if (led->port != NULL) {
			gpio_pin_set_dt(led, 1);
		}
	}
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
 * preventing a use-after-free if the peer disconnects mid-send. */
K_MUTEX_DEFINE(conn_mutex);

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

static const struct bt_le_adv_param adv_param_slow = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN,
	BT_GAP_ADV_SLOW_INT_MIN,   /* 1.0 s */
	BT_GAP_ADV_SLOW_INT_MAX,   /* 1.2 s */
	NULL);

static struct k_work_delayable adv_slow_work;

static void advertising_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		set_status_leds(STATUS_ERROR);
		return;
	}
	LOG_INF("Advertising as \"%s\" (fast)", device_name);
	adv_slow_phase = false;   /* brisk LED blink during the fast phase */
	set_status_leds(STATUS_ADVERTISING);
	k_work_reschedule(&adv_slow_work, ADV_FAST_DURATION);
}

/* Restart advertising at the slow interval. Skipped if a connection arrived in
 * the meantime (on_connected also cancels this work, but guard the race). */
static void adv_slow_handler(struct k_work *work)
{
	int err;

	k_mutex_lock(&conn_mutex, K_FOREVER);
	bool connected = current_conn != NULL;
	k_mutex_unlock(&conn_mutex);
	if (connected) {
		return;
	}

	err = bt_le_adv_stop();
	if (err) {
		LOG_WRN("adv stop failed (%d)", err);
		return;
	}
	err = bt_le_adv_start(&adv_param_slow, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("slow advertising failed to start (err %d)", err);
		set_status_leds(STATUS_ERROR);
		return;
	}
	LOG_INF("Advertising (slow)");
	adv_slow_phase = true;   /* LED drops to the lazy blink */
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		/* Advertising stopped when the connection attempt began; bring
		 * it back so a failed/rejected peer can't leave us invisible. */
		advertising_start();
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected: %s", addr);

	/* Advertising stopped on connect; don't let the fast->slow switch fire. */
	k_work_cancel_delayable(&adv_slow_work);

	k_mutex_lock(&conn_mutex, K_FOREVER);
	current_conn = bt_conn_ref(conn);
	k_mutex_unlock(&conn_mutex);

	set_status_leds(STATUS_CONNECTED);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason %u)", reason);

	k_mutex_lock(&conn_mutex, K_FOREVER);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	k_mutex_unlock(&conn_mutex);

	/* Drop anything buffered so the next client starts on a clean stream. */
	ring_buf_reset(&uart_rx_ringbuf);

	advertising_start();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
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
	irq_unlock(key);

	if (uart_tx(uart_dev, uart_tx_buf, len, SYS_FOREVER_US) != 0) {
		uart_tx_in_progress = false;
	}
}

/* Phone -> device: data written to the NUS RX characteristic goes out UART1. */
static void nus_received(struct bt_conn *conn, const uint8_t *const data,
			 uint16_t len)
{
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
	case UART_TX_ABORTED:
		/* Current chunk finished; send the next one if queued. */
		uart_tx_in_progress = false;
		uart_tx_kick();
		break;

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
			ring_buf_put(&uart_rx_ringbuf, proc_buf, out_len);
			k_sem_give(&rx_data_ready);
		}
		break;
	}

	case UART_RX_BUF_REQUEST:
		/* Provide the next buffer for double-buffered reception. */
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
		/* Reception stopped (e.g. out of buffers): restart it. */
		if (k_mem_slab_alloc(&uart_rx_slab, (void **)&buf, K_NO_WAIT) == 0) {
			err = uart_rx_enable(dev, buf, UART_BUF_SIZE,
					     UART_RX_TIMEOUT_US);
			if (err) {
				k_mem_slab_free(&uart_rx_slab, buf);
			}
		}
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

static void ble_write_thread(void)
{
	uint8_t *chunk;
	uint32_t claimed;
	uint16_t max_send;
	int err;

	for (;;) {
		k_sem_take(&rx_data_ready, K_FOREVER);

		while (!ring_buf_is_empty(&uart_rx_ringbuf)) {
			struct bt_conn *conn = NULL;

			/* Take our own reference so the connection object can't be
			 * freed by a concurrent disconnect while we use it. */
			k_mutex_lock(&conn_mutex, K_FOREVER);
			if (current_conn) {
				conn = bt_conn_ref(current_conn);
			}
			k_mutex_unlock(&conn_mutex);

			if (!conn) {
				/* Nobody listening: discard buffered data. */
				ring_buf_reset(&uart_rx_ringbuf);
				break;
			}

			/* Notification payload is limited to ATT_MTU - 3. */
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
	}
}

K_THREAD_DEFINE(ble_write_thread_id, 2048, ble_write_thread, NULL, NULL, NULL,
		7, 0, 0);

/* --- main ---------------------------------------------------------------- */

int main(void)
{
	int err;

	LOG_INF("nrfProxy: UART1 -> BLE NUS bridge starting");

	leds_init();
	k_work_init_delayable(&adv_slow_work, adv_slow_handler);

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

	/* Apply the resolved name to the GAP Device Name characteristic and the
	 * advertising payload (both must happen after bt_enable). */
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

	advertising_start();

	return 0;
}
