/*
 * Group B — the interception hooks (ADD_TESTING_PLAN.md).
 *
 * The hooks are pass-through stubs today, so what is worth pinning is not their
 * (absent) behaviour but their *contract* -- the one the real filter logic will
 * be written against: never write past out_size, return exactly what you wrote,
 * 0 means drop. Both hooks are held to it identically, hence the table.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "proxy_core.h"

typedef size_t (*hook_fn)(const uint8_t *in, size_t in_len,
			  uint8_t *out, size_t out_size);

static const struct {
	const char *name;
	hook_fn fn;
} hooks[] = {
	{ "on_uart_rx", on_uart_rx },
	{ "on_ble_rx", on_ble_rx },
};

/* Written either side of the expected output so an overrun is visible. */
#define CANARY 0xA5

/* Deliberately not memset: a constant fill would hide an off-by-one copy. */
static void fill_pattern(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)(i * 31u + 7u);
	}
}

static void assert_canaries(const uint8_t *buf, size_t from, size_t to,
			    const char *hook)
{
	for (size_t i = from; i < to; i++) {
		zassert_equal(buf[i], CANARY,
			      "%s: wrote past its output limit at byte %u",
			      hook, (unsigned int)i);
	}
}

/* B1 -- an oversized input is clamped to out_size, not overrun. */
ZTEST(proxy_core_hooks, test_output_clamped_to_out_size)
{
	static uint8_t in[PROC_BUF_SIZE + 64];
	static uint8_t out[PROC_BUF_SIZE + 64];

	fill_pattern(in, sizeof(in));

	for (size_t h = 0; h < ARRAY_SIZE(hooks); h++) {
		memset(out, CANARY, sizeof(out));

		size_t n = hooks[h].fn(in, sizeof(in), out, PROC_BUF_SIZE);

		zassert_equal(n, PROC_BUF_SIZE, "%s: returned %u, want %u",
			      hooks[h].name, (unsigned int)n,
			      (unsigned int)PROC_BUF_SIZE);
		zassert_mem_equal(out, in, PROC_BUF_SIZE,
				  "%s: clamped output differs from input",
				  hooks[h].name);
		assert_canaries(out, PROC_BUF_SIZE, sizeof(out), hooks[h].name);
	}
}

/* B2 -- the pass-through contract: everything in, byte-identical, nothing else. */
ZTEST(proxy_core_hooks, test_passthrough_is_byte_identical)
{
	static uint8_t in[64];
	static uint8_t out[PROC_BUF_SIZE];

	fill_pattern(in, sizeof(in));

	for (size_t h = 0; h < ARRAY_SIZE(hooks); h++) {
		memset(out, CANARY, sizeof(out));

		size_t n = hooks[h].fn(in, sizeof(in), out, sizeof(out));

		zassert_equal(n, sizeof(in), "%s: returned %u, want %u",
			      hooks[h].name, (unsigned int)n,
			      (unsigned int)sizeof(in));
		zassert_mem_equal(out, in, sizeof(in),
				  "%s: output differs from input", hooks[h].name);
		assert_canaries(out, sizeof(in), sizeof(out), hooks[h].name);
	}
}

/* B3 -- zero-length input returns 0 and touches nothing. */
ZTEST(proxy_core_hooks, test_zero_length_input_writes_nothing)
{
	uint8_t in[1] = { 0x42 };
	uint8_t out[16];

	for (size_t h = 0; h < ARRAY_SIZE(hooks); h++) {
		memset(out, CANARY, sizeof(out));

		size_t n = hooks[h].fn(in, 0, out, sizeof(out));

		zassert_equal(n, 0, "%s: returned %u for empty input",
			      hooks[h].name, (unsigned int)n);
		assert_canaries(out, 0, sizeof(out), hooks[h].name);
	}
}

/* B4 -- the boundary: an input exactly filling the scratch buffer is kept whole. */
ZTEST(proxy_core_hooks, test_input_exactly_proc_buf_size)
{
	static uint8_t in[PROC_BUF_SIZE];
	static uint8_t out[PROC_BUF_SIZE + 16];

	fill_pattern(in, sizeof(in));

	for (size_t h = 0; h < ARRAY_SIZE(hooks); h++) {
		memset(out, CANARY, sizeof(out));

		size_t n = hooks[h].fn(in, PROC_BUF_SIZE, out, PROC_BUF_SIZE);

		zassert_equal(n, PROC_BUF_SIZE, "%s: returned %u, want %u",
			      hooks[h].name, (unsigned int)n,
			      (unsigned int)PROC_BUF_SIZE);
		zassert_mem_equal(out, in, PROC_BUF_SIZE,
				  "%s: output differs from input", hooks[h].name);
		assert_canaries(out, PROC_BUF_SIZE, sizeof(out), hooks[h].name);
	}
}

/*
 * B5 -- placeholder, deliberately skipped rather than deleted.
 *
 * The hooks are the project's designated extension point and are still stubs.
 * When real filter logic lands, the two behaviours it must honour are (a) growing
 * the data, still bounded by out_size, and (b) returning 0 to drop a chunk. Both
 * are untestable against a memcpy, so this records the contract where whoever
 * writes that logic will trip over it. Skipped, not commented out: a skip is
 * visible in the test report; a comment is not.
 */
ZTEST(proxy_core_hooks, test_grow_and_drop_paths)
{
	ztest_test_skip();
}

ZTEST_SUITE(proxy_core_hooks, NULL, NULL, NULL, NULL, NULL);
