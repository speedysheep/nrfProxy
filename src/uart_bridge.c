/*
 * uart_bridge — UART async driver glue + the RX/TX ring buffers.
 * See uart_bridge.h for the concurrency contract.
 */

#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "uart_bridge.h"

LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

#define UART_BUF_SIZE      64       /* size of each async RX buffer */
#define UART_RX_TIMEOUT_US 50000U   /* flush partial data after 50 ms idle */

/* Pool of RX buffers handed to the async UART driver, and a ring buffer that the
 * consumer thread drains.
 *
 * Sizing: 4 slab buffers because the driver holds up to two at a time
 * (double-buffering) and the release of a finished buffer can lag its
 * replacement request — 2 would deadlock the RX path on that lag, 4 gives
 * margin. The 2048-byte ring is ~180 ms of a sustained 115200 stream: enough to
 * ride out a BLE buffer shortage without dropping, small enough that stale data
 * can't build up a noticeable forwarding delay. */
K_MEM_SLAB_DEFINE(uart_rx_slab, UART_BUF_SIZE, 4, 4);
RING_BUF_DECLARE(uart_rx_ringbuf, 2048);

/* TX path: bytes received from the phone are queued here and fed out the UART.
 * uart_tx() can only have one transfer in flight, so we stage a contiguous chunk
 * in uart_tx_buf and start the next chunk on UART_TX_DONE. */
RING_BUF_DECLARE(uart_tx_ringbuf, 2048);
static uint8_t uart_tx_buf[UART_BUF_SIZE];
static bool uart_tx_in_progress;

static const struct device *bridge_dev;
static struct k_sem *rx_ready_sem;

/* Start a UART transmission if one isn't already running. Safe to call from both
 * thread context (a queued send) and ISR context (UART_TX_DONE); the in-progress
 * flag is checked under irq_lock so only one transfer starts. */
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

	if (uart_tx(bridge_dev, uart_tx_buf, len, SYS_FOREVER_US) != 0) {
		/* The staged chunk is deliberately dropped rather than re-queued:
		 * a failing uart_tx() here means the driver is in a state where
		 * retrying inline would likely fail too, and this stream is
		 * repetitive/loss-tolerant by design. Clearing the flag lets the
		 * next send (or TX_DONE) kick the pipeline back to life. */
		uart_tx_in_progress = false;
	}
}

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

	case UART_RX_RDY:
		/* Raw bytes straight into the ring: the interception hook runs in
		 * the consumer thread, not here. An ISR is the wrong place for the
		 * filter/framing logic the hook exists for. */
		ring_buf_put(&uart_rx_ringbuf, evt->data.rx.buf + evt->data.rx.offset,
			     evt->data.rx.len);
		if (evt->data.rx.len > 0) {
			k_sem_give(rx_ready_sem);
		}
		break;

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

/* --- Public API ----------------------------------------------------------- */

int uart_bridge_init(const struct device *uart_dev, struct k_sem *rx_data_ready)
{
	uint8_t *buf;
	int err;

	bridge_dev = uart_dev;
	rx_ready_sem = rx_data_ready;

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

void uart_bridge_send(const uint8_t *data, size_t len)
{
	uint32_t queued = ring_buf_put(&uart_tx_ringbuf, data, len);

	if (queued < len) {
		LOG_WRN("UART TX buffer full, dropped %u bytes",
			(unsigned int)(len - queued));
	}

	uart_tx_kick();
}

bool uart_bridge_rx_is_empty(void)
{
	return ring_buf_is_empty(&uart_rx_ringbuf);
}

uint32_t uart_bridge_rx_claim(uint8_t **buf, uint32_t max)
{
	return ring_buf_get_claim(&uart_rx_ringbuf, buf, max);
}

void uart_bridge_rx_finish(uint32_t consumed)
{
	ring_buf_get_finish(&uart_rx_ringbuf, consumed);
}

void uart_bridge_rx_drain(void)
{
	uint8_t *buf;
	uint32_t len;

	while ((len = ring_buf_get_claim(&uart_rx_ringbuf, &buf, UINT32_MAX)) > 0) {
		ring_buf_get_finish(&uart_rx_ringbuf, len);
	}
}
