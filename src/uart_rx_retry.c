#include <errno.h>

#include "uart_rx_retry.h"

bool uart_rx_enable_succeeded(int enable_err)
{
	return enable_err == 0 || enable_err == -EBUSY;
}

bool uart_rx_should_schedule_retry(bool alloc_ok, int enable_err)
{
	if (!alloc_ok) {
		return true;
	}
	return !uart_rx_enable_succeeded(enable_err);
}

bool uart_rx_retry_warn_once(bool *latched)
{
	if (*latched) {
		return false;
	}
	*latched = true;
	return true;
}

void uart_rx_retry_clear_latch(bool *latched)
{
	*latched = false;
}
