/*
 * Group E — the UART data path (ADD_TESTING_PLAN.md), driving the real
 * src/uart_bridge.c against Zephyr's emulated UART on native_sim.
 *
 * The invariants under test are the ones with bugs behind them: bytes survive
 * the ISR->ring->consumer path in order, a full ring truncates instead of
 * corrupting, a send larger than the staging buffer still arrives whole (which
 * only happens if the TX_DONE chaining works), and draining from the consumer
 * side is safe against a concurrent producer -- the reason ring_buf_reset() is
 * forbidden.
 *
 * Timing: the emulated driver moves data through its own work queue, and
 * uart_bridge asks for a 50 ms RX idle timeout, so the tests wait on the
 * semaphore and then settle rather than assuming synchronous delivery.
 */

#include <string.h>

#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "uart_bridge.h"

#define UART_NODE DT_NODELABEL(euart0)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);

static K_SEM_DEFINE(rx_data_ready, 0, 1);

/* The RX ring inside uart_bridge (kept in step with RING_BUF_DECLARE there). */
#define RX_RING_CAPACITY 2048
/* uart_bridge stages TX in chunks of this size; sends larger than it must be
 * chained across several transfers. */
#define TX_STAGING_SIZE 64

/* Longer than the 50 ms RX idle timeout plus the emulator's work-queue hop. */
#define SETTLE_MS  300
#define WAIT_MS    1000

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)(seed + i * 7u);
	}
}

/* Feed the emulator, which accepts only as much as its RX FIFO has room for. */
static size_t feed(const uint8_t *data, size_t len)
{
	size_t fed = 0;

	while (fed < len) {
		uint32_t n = uart_emul_put_rx_data(uart_dev, data + fed, len - fed);

		if (n == 0) {
			/* FIFO full: let the driver drain it into the ring. */
			k_sleep(K_MSEC(20));
			continue;
		}
		fed += n;
	}
	return fed;
}

/* Drain the RX ring into `out`, consumer-side, exactly as ble_write_thread does. */
static size_t drain_into(uint8_t *out, size_t out_size)
{
	size_t total = 0;

	while (!uart_bridge_rx_is_empty() && total < out_size) {
		uint8_t *chunk;
		uint32_t claimed = uart_bridge_rx_claim(&chunk, out_size - total);

		if (claimed == 0) {
			break;
		}
		memcpy(out + total, chunk, claimed);
		total += claimed;
		uart_bridge_rx_finish(claimed);
	}
	return total;
}

static void *suite_setup(void)
{
	zassert_true(device_is_ready(uart_dev), "emulated UART not ready");
	zassert_ok(uart_bridge_init(uart_dev, &rx_data_ready),
		   "uart_bridge_init failed");
	return NULL;
}

/* Each test starts from a quiet bridge: no buffered bytes either side. */
static void test_before(void *fixture)
{
	ARG_UNUSED(fixture);

	k_sleep(K_MSEC(SETTLE_MS));
	uart_emul_flush_rx_data(uart_dev);
	uart_emul_flush_tx_data(uart_dev);
	uart_bridge_rx_drain();
	k_sem_reset(&rx_data_ready);
}

/* E1 -- injected bytes reach the ring intact, in order, and wake the consumer. */
ZTEST(uart_bridge, test_rx_data_arrives_intact_and_in_order)
{
	static const char part1[] = "hello ";
	static const char part2[] = "world";
	uint8_t out[64];

	feed((const uint8_t *)part1, strlen(part1));
	feed((const uint8_t *)part2, strlen(part2));

	zassert_ok(k_sem_take(&rx_data_ready, K_MSEC(WAIT_MS)),
		   "rx_data_ready was never given: the consumer would sleep "
		   "through incoming serial data");
	k_sleep(K_MSEC(SETTLE_MS));

	size_t n = drain_into(out, sizeof(out));

	zassert_equal(n, 11, "got %u bytes, want 11", (unsigned int)n);
	zassert_mem_equal(out, "hello world", 11,
			  "bytes were reordered or corrupted in the ring");
}

/* A chunk larger than one 64-byte RX buffer must still cross intact -- this is
 * the double-buffering / slab hand-off path. */
ZTEST(uart_bridge, test_rx_spans_multiple_driver_buffers)
{
	static uint8_t in[512];
	static uint8_t out[sizeof(in)];

	fill_pattern(in, sizeof(in), 0x10);
	feed(in, sizeof(in));

	zassert_ok(k_sem_take(&rx_data_ready, K_MSEC(WAIT_MS)), "no wakeup");
	k_sleep(K_MSEC(SETTLE_MS));

	size_t n = drain_into(out, sizeof(out));

	zassert_equal(n, sizeof(in), "got %u of %u bytes across buffer swaps",
		      (unsigned int)n, (unsigned int)sizeof(in));
	zassert_mem_equal(out, in, sizeof(in), "data corrupted across buffers");
}

/* E2 -- a full ring truncates on the put side. The bytes that made it must be
 * an intact prefix: dropping the tail is the design, corrupting is not. */
ZTEST(uart_bridge, test_rx_ring_overflow_truncates_without_corruption)
{
	static uint8_t in[RX_RING_CAPACITY + 1024];
	static uint8_t out[sizeof(in)];

	fill_pattern(in, sizeof(in), 0x80);
	feed(in, sizeof(in));
	k_sleep(K_MSEC(SETTLE_MS * 2));

	size_t n = drain_into(out, sizeof(out));

	zassert_true(n <= RX_RING_CAPACITY,
		     "ring returned %u bytes, more than its %u capacity",
		     (unsigned int)n, (unsigned int)RX_RING_CAPACITY);
	zassert_true(n > 0, "overflow lost everything, not just the excess");
	zassert_mem_equal(out, in, n,
			  "the bytes that survived must be an intact prefix");

	/* And the ring recovers: it is not wedged by the overflow. */
	test_before(NULL);
	static const char after[] = "still alive";

	feed((const uint8_t *)after, strlen(after));
	zassert_ok(k_sem_take(&rx_data_ready, K_MSEC(WAIT_MS)),
		   "RX died after an overflow");
	k_sleep(K_MSEC(SETTLE_MS));

	size_t m = drain_into(out, sizeof(out));

	zassert_equal(m, strlen(after), "got %u bytes after overflow",
		      (unsigned int)m);
	zassert_mem_equal(out, after, strlen(after), "post-overflow data corrupt");
}

/* E3 -- a send larger than the staging buffer arrives whole and in order, which
 * can only happen if each chunk is chained from the previous TX_DONE. */
ZTEST(uart_bridge, test_tx_larger_than_staging_buffer_is_chained)
{
	static uint8_t in[TX_STAGING_SIZE * 3 + 7];   /* deliberately not a multiple */
	static uint8_t out[sizeof(in)];

	fill_pattern(in, sizeof(in), 0x33);
	uart_bridge_send(in, sizeof(in));
	k_sleep(K_MSEC(SETTLE_MS));

	uint32_t n = uart_emul_get_tx_data(uart_dev, out, sizeof(out));

	zassert_equal(n, sizeof(in),
		      "%u of %u bytes reached the wire: the TX_DONE chain stopped "
		      "after the first staged chunk",
		      (unsigned int)n, (unsigned int)sizeof(in));
	zassert_mem_equal(out, in, sizeof(in), "TX bytes reordered or corrupted");
}

/* Back-to-back sends must queue behind the in-flight transfer, not interleave:
 * only one uart_tx() may be outstanding. */
ZTEST(uart_bridge, test_back_to_back_sends_preserve_order)
{
	static uint8_t first[100];
	static uint8_t second[100];
	static uint8_t out[sizeof(first) + sizeof(second)];

	fill_pattern(first, sizeof(first), 0x01);
	fill_pattern(second, sizeof(second), 0xC0);

	uart_bridge_send(first, sizeof(first));
	uart_bridge_send(second, sizeof(second));
	k_sleep(K_MSEC(SETTLE_MS));

	uint32_t n = uart_emul_get_tx_data(uart_dev, out, sizeof(out));

	zassert_equal(n, sizeof(out), "got %u of %u bytes", (unsigned int)n,
		      (unsigned int)sizeof(out));
	zassert_mem_equal(out, first, sizeof(first), "first send corrupted");
	zassert_mem_equal(out + sizeof(first), second, sizeof(second),
			  "second send overtook or corrupted the first");
}

/* E7 -- draining while the producer is still delivering. This is the regression
 * that made ring_buf_reset() forbidden: a reset from a non-consumer context
 * rewrites both indices and corrupts the ring against a concurrent put. Draining
 * from the consumer side with claim/finish must be safe, and the ring must still
 * work afterwards. */
ZTEST(uart_bridge, test_drain_concurrent_with_producer)
{
	static uint8_t in[256];
	static uint8_t out[RX_RING_CAPACITY];

	fill_pattern(in, sizeof(in), 0x55);

	for (int round = 0; round < 20; round++) {
		feed(in, sizeof(in));
		uart_bridge_rx_drain();   /* races the emulator's delivery */
	}
	k_sleep(K_MSEC(SETTLE_MS));
	uart_bridge_rx_drain();

	zassert_true(uart_bridge_rx_is_empty(),
		     "the ring should be empty after a settled drain");

	/* The indices must still be sane: fresh data crosses intact. */
	k_sem_reset(&rx_data_ready);
	static const char after[] = "indices intact";

	feed((const uint8_t *)after, strlen(after));
	zassert_ok(k_sem_take(&rx_data_ready, K_MSEC(WAIT_MS)),
		   "RX stopped after draining against a live producer");
	k_sleep(K_MSEC(SETTLE_MS));

	size_t n = drain_into(out, sizeof(out));

	zassert_equal(n, strlen(after),
		      "got %u bytes after concurrent drain, want %u -- the ring "
		      "indices were corrupted", (unsigned int)n,
		      (unsigned int)strlen(after));
	zassert_mem_equal(out, after, strlen(after),
			  "data after a concurrent drain is corrupt");
}

/* A drain with nothing buffered is a no-op, not an underflow. */
ZTEST(uart_bridge, test_drain_when_empty_is_harmless)
{
	uart_bridge_rx_drain();
	uart_bridge_rx_drain();
	zassert_true(uart_bridge_rx_is_empty(), "empty ring reported non-empty");
}

/* Claiming more than is buffered returns only what is there, and finishing 0
 * keeps the data for the next claim -- the keep-and-retry path. */
ZTEST(uart_bridge, test_claim_finish_zero_keeps_data)
{
	static const char msg[] = "keepme";
	uint8_t *chunk;

	feed((const uint8_t *)msg, strlen(msg));
	zassert_ok(k_sem_take(&rx_data_ready, K_MSEC(WAIT_MS)), "no wakeup");
	k_sleep(K_MSEC(SETTLE_MS));

	uint32_t claimed = uart_bridge_rx_claim(&chunk, 1024);

	zassert_equal(claimed, strlen(msg), "claimed %u, want %u",
		      (unsigned int)claimed, (unsigned int)strlen(msg));
	uart_bridge_rx_finish(0);

	zassert_false(uart_bridge_rx_is_empty(),
		      "finish(0) must keep the data, as the -ENOMEM retry needs");

	uint8_t out[32];
	size_t n = drain_into(out, sizeof(out));

	zassert_equal(n, strlen(msg), "kept data changed size");
	zassert_mem_equal(out, msg, strlen(msg), "kept data corrupted");
}

ZTEST_SUITE(uart_bridge, NULL, suite_setup, test_before, NULL, NULL);
