/*
 * uart_bridge — the UART side of the proxy: the async driver glue and the two
 * ring buffers that decouple the serial byte rate from BLE throughput.
 *
 * Deliberately free of any Bluetooth dependency, so it can be driven on
 * native_sim against an emulated UART (tests/integration/uart_bridge) with no
 * BLE stack. main.c owns everything BLE and calls in from both directions.
 *
 * Concurrency contract — this is the load-bearing part, and every rule here
 * exists because breaking it was a real bug:
 *
 *  - The RX ring is strictly single-producer (UART ISR) / single-consumer
 *    (whichever ONE thread calls the rx_* functions; in the app, ble_write_thread).
 *    Nothing else may touch it.
 *  - Consequently uart_bridge_rx_drain() flushes from the *consumer* side using
 *    claim/finish. ring_buf_reset() is forbidden: it rewrites both indices and
 *    corrupts the buffer against a concurrent ISR put.
 *  - uart_bridge_send() is the TX ring's only producer. The consumer is
 *    uart_tx_kick(), which runs from both thread and ISR context, so the
 *    in-progress flag is guarded with irq_lock().
 */
#ifndef UART_BRIDGE_H_
#define UART_BRIDGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

/*
 * Start receiving on `uart_dev`. `rx_data_ready` is given whenever RX bytes land
 * (binary semaphore: it means "there may be data", and the consumer is expected
 * to drain completely per wakeup).
 *
 * Returns 0, or a negative errno if the device is not ready or the async API is
 * unavailable (-ENOSYS means UART_<n>_INTERRUPT_DRIVEN got turned back on for
 * this instance -- see CLAUDE.md).
 */
int uart_bridge_init(const struct device *uart_dev, struct k_sem *rx_data_ready);

/* Phone -> serial: queue `len` bytes for transmission and start the pipeline.
 * Whatever does not fit is dropped (the stream is loss-tolerant by design) and
 * logged. Safe from thread context. */
void uart_bridge_send(const uint8_t *data, size_t len);

/* --- RX consumption. Consumer thread only (see the contract above). -------- */

bool uart_bridge_rx_is_empty(void);

/* Claim up to `max` contiguous bytes without consuming them; returns the count
 * and points `buf` at them. Follow with uart_bridge_rx_finish(). */
uint32_t uart_bridge_rx_claim(uint8_t **buf, uint32_t max);

/* Consume `consumed` of the claimed bytes (0 keeps them all for a later claim). */
void uart_bridge_rx_finish(uint32_t consumed);

/* Discard everything buffered, from the consumer side. */
void uart_bridge_rx_drain(void);

#endif /* UART_BRIDGE_H_ */
