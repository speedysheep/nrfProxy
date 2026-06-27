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

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
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

/* --- BLE ----------------------------------------------------------------- */

#define DEVICE_NAME       CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN   (sizeof(DEVICE_NAME) - 1)

static struct bt_conn *current_conn;
/* Guards current_conn so the writer thread can take a reference before use,
 * preventing a use-after-free if the peer disconnects mid-send. */
K_MUTEX_DEFINE(conn_mutex);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void advertising_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}
	LOG_INF("Advertising as \"%s\"", DEVICE_NAME);
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

	k_mutex_lock(&conn_mutex, K_FOREVER);
	current_conn = bt_conn_ref(conn);
	k_mutex_unlock(&conn_mutex);
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

	err = uart_init();
	if (err) {
		LOG_ERR("UART init failed (%d)", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return 0;
	}
	LOG_INF("Bluetooth initialized");

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("bt_nus_init failed (%d)", err);
		return 0;
	}

	advertising_start();

	return 0;
}
