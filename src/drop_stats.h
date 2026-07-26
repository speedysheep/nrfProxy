/*
 * Silent-drop accounting (ISR-safe counters + thread-context report policy).
 */
#ifndef DROP_STATS_H_
#define DROP_STATS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes not written by ring_buf_put (attempted - queued). */
uint32_t drop_stats_rx_overflow(uint32_t attempted, uint32_t queued);

/* True when a periodic report should fire (changed since last AND interval elapsed). */
bool drop_stats_should_report(uint32_t now_ms, uint32_t last_report_ms,
			      uint32_t interval_ms, bool counters_changed);

#ifdef __cplusplus
}
#endif

#endif /* DROP_STATS_H_ */
