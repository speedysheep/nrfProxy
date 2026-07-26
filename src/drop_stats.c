#include "drop_stats.h"

uint32_t drop_stats_rx_overflow(uint32_t attempted, uint32_t queued)
{
	return (queued < attempted) ? (attempted - queued) : 0;
}

bool drop_stats_should_report(uint32_t now_ms, uint32_t last_report_ms,
			      uint32_t interval_ms, bool counters_changed)
{
	if (!counters_changed) {
		return false;
	}
	return (now_ms - last_report_ms) >= interval_ms;
}
