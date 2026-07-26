/*
 * UART async RX recovery policy (pure logic — host-testable without Zephyr).
 *
 * When UART_RX_DISABLED fails to re-enable, a delayed work item retries until
 * success. -EBUSY means another path already re-enabled — treat as success.
 */
#ifndef UART_RX_RETRY_H_
#define UART_RX_RETRY_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* enable_err from uart_rx_enable(); 0 and -EBUSY both mean RX is live. */
bool uart_rx_enable_succeeded(int enable_err);

/* True when the ISR/work path should schedule (or re-schedule) the retry work. */
bool uart_rx_should_schedule_retry(bool alloc_ok, int enable_err);

/*
 * Once-per-outage warn latch: returns true the first time an outage is
 * observed; subsequent failures return false until cleared on success.
 */
bool uart_rx_retry_warn_once(bool *latched);

void uart_rx_retry_clear_latch(bool *latched);

#ifdef __cplusplus
}
#endif

#endif /* UART_RX_RETRY_H_ */
