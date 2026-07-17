/*
 * Group D — link, advertising and send policy (ADD_TESTING_PLAN.md).
 *
 * These are the predicates behind the three BLE-lifecycle invariants in
 * ARCHITECTURE.md section 5, each of which was a field bug: a device unreachable
 * until reboot after the first disconnect, a spurious error LED from a second
 * competing advertiser, and data flowing before the link encrypted.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "proxy_core.h"

/* --- D1: when advertising may start --------------------------------------- */

ZTEST(proxy_core_policy, test_should_start_adv_all_combinations)
{
	const struct {
		bool connected;
		bool adv_active;
		bool want;
		const char *why;
	} cases[] = {
		{ false, false, true,
		  "idle: nothing is using the advertiser" },
		{ true, false, false,
		  "connected: with BT_MAX_CONN=1 a connectable start fails "
		  "-ENOMEM and the device is unreachable until reboot" },
		{ false, true, false,
		  "already advertising: a second start returns -EALREADY" },
		{ true, true, false,
		  "connected wins regardless of the advertiser flag" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct proxy_link_state state = {
			.connected = cases[i].connected,
			.adv_active = cases[i].adv_active,
		};

		zassert_equal(proxy_should_start_adv(&state), cases[i].want,
			      "connected=%d adv_active=%d: %s",
			      (int)cases[i].connected, (int)cases[i].adv_active,
			      cases[i].why);
	}
}

/* --- D2: when NUS data may flow ------------------------------------------- */

ZTEST(proxy_core_policy, test_may_forward_all_combinations)
{
	const struct {
		bool connected;
		bool link_secure;
		bool want;
	} cases[] = {
		{ false, false, false },
		{ true,  false, false },   /* the pairing lock's actual gate */
		{ false, true,  false },   /* stale flag, no peer: never forward */
		{ true,  true,  true },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct proxy_link_state state = {
			.connected = cases[i].connected,
			.link_secure = cases[i].link_secure,
		};

		zassert_equal(proxy_may_forward(&state), cases[i].want,
			      "connected=%d link_secure=%d must%s forward",
			      (int)cases[i].connected, (int)cases[i].link_secure,
			      cases[i].want ? "" : " not");
	}
}

/* --- D3: the security watchdog window ------------------------------------- */

/* Pins the values current at implementation time. If TODO_ARCHITECTURE Task 2
 * collapses both windows to 60 s, update the two exact assertions -- but the
 * relational ones below must keep holding whatever the values become. */
ZTEST(proxy_core_policy, test_security_window_per_mode)
{
	zassert_equal(proxy_security_window_ms(true), 10000U,
		      "locked mode: the bonded phone encrypts in a couple of seconds");
	zassert_equal(proxy_security_window_ms(false), 60000U,
		      "pairing mode: must cover a human accepting Android's dialog");
}

ZTEST(proxy_core_policy, test_pairing_window_stays_long_enough)
{
	/* Do not shorten this. Disconnecting while SMP is in flight aborts the
	 * procedure, which Android reports as "couldn't pair: incorrect PIN" --
	 * observed on hardware with a flat 10 s window. It costs nothing: with
	 * no bond stored there is no owner to protect. */
	zassert_true(proxy_security_window_ms(false) >= 30000U,
		     "the pairing window must leave time to tap the dialog");
	zassert_true(proxy_security_window_ms(false) >= proxy_security_window_ms(true),
		     "pairing must never be the stricter window");
	zassert_true(proxy_security_window_ms(true) > 0U,
		     "a zero window would drop every link before it could encrypt");
}

/* --- D4: notification chunk sizing ---------------------------------------- */

ZTEST(proxy_core_policy, test_nus_chunk_limit)
{
	const struct {
		uint16_t mtu;
		uint16_t want;
		const char *why;
	} cases[] = {
		{ 23,  20,  "the minimum ATT MTU, minus opcode and handle" },
		{ 247, 244, "the MTU prj.conf negotiates for" },
		{ 3,   20,  "a nonsense MTU falls back, never underflows to 0" },
		{ 0,   20,  "an unreported MTU falls back" },
		{ 4,   1,   "just above the header: one payload byte" },
		{ 65535, 65532, "no overflow at the top of the range" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_equal(proxy_nus_chunk_limit(cases[i].mtu), cases[i].want,
			      "mtu %u -> %u, want %u (%s)", cases[i].mtu,
			      proxy_nus_chunk_limit(cases[i].mtu), cases[i].want,
			      cases[i].why);
	}
}

/* --- D5: send-error policy ------------------------------------------------ */

ZTEST(proxy_core_policy, test_send_result_classification)
{
	const struct {
		int err;
		enum proxy_send_verdict want;
		const char *why;
	} cases[] = {
		{ 0,         PROXY_SEND_CONSUMED, "copied into the GATT buffer" },
		{ -ENOMEM,   PROXY_SEND_RETRY,    "no TX buffers right now" },
		{ -EAGAIN,   PROXY_SEND_RETRY,    "try again shortly" },
		{ -ENOTCONN, PROXY_SEND_DROP,     "peer gone" },
		{ -EINVAL,   PROXY_SEND_DROP,     "not subscribed" },
		{ -EPIPE,    PROXY_SEND_DROP,     "broken link" },
		{ -EIO,      PROXY_SEND_DROP,     "anything unknown drops, never wedges" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_equal(proxy_send_result(cases[i].err), cases[i].want,
			      "err %d must be verdict %d (%s)", cases[i].err,
			      (int)cases[i].want, cases[i].why);
	}
}

/* --- D6: event sequences -------------------------------------------------- */
/*
 * The three field bugs were all about *ordering*, not about a single predicate,
 * so replay the orderings.
 *
 * Scope, honestly: apply_event() below models what main.c's callbacks do to the
 * mutex-guarded trio. It is a model, not the real thing -- it cannot catch
 * main.c wiring an event to the wrong callback (that is what BabbleSim F1/F2 are
 * for). What it does catch is the decision logic being changed so that a known
 * bad ordering becomes allowed again, which is exactly how these bugs arrived.
 * Each event names the main.c function it mirrors; keep them in step.
 */

enum proxy_event {
	EV_ADV_STARTED,           /* advertising_start(): bt_le_adv_start() == 0 */
	EV_ADV_STOPPED_FOR_SLOW,  /* adv_slow_handler(): the stop->start gap */
	EV_CONNECTED,             /* on_connected(err == 0) */
	EV_CONNECT_FAILED,        /* on_connected(err != 0) */
	EV_DISCONNECTED,          /* on_disconnected() */
	EV_SECURED,               /* on_security_changed(level >= L2) */
};

static void apply_event(struct proxy_link_state *state, enum proxy_event event)
{
	switch (event) {
	case EV_ADV_STARTED:
		state->adv_active = true;
		break;
	case EV_ADV_STOPPED_FOR_SLOW:
		/* Deliberately no state change: adv_slow_handler holds
		 * conn_mutex with adv_active still true across the stop->start
		 * gap, precisely so a recycled callback firing in the gap
		 * cannot start a competing advertiser. */
		break;
	case EV_CONNECTED:
		state->connected = true;
		state->adv_active = false;  /* the attempt consumed the advertiser */
		state->link_secure = false; /* no data until the link encrypts */
		break;
	case EV_CONNECT_FAILED:
		state->adv_active = false;  /* cleared in both on_connected branches */
		break;
	case EV_DISCONNECTED:
		state->connected = false;
		state->link_secure = false;
		break;
	case EV_SECURED:
		state->link_secure = true;
		break;
	}
}

#define MAX_EVENTS 6

/* Each case ends at the point the recycled callback would call
 * advertising_start(), and asserts whether it may proceed. */
static const struct {
	const char *name;
	enum proxy_event events[MAX_EVENTS];
	size_t event_count;
	bool start_allowed;
	const char *why;
} adv_sequences[] = {
	{
		"boot",
		{ 0 }, 0, true,
		"nothing running yet",
	},
	{
		"connect -> disconnect -> recycled",
		{ EV_ADV_STARTED, EV_CONNECTED, EV_DISCONNECTED }, 3, true,
		"field bug #1: the restart must happen here, at recycled -- from "
		"on_disconnected the conn object is still allocated and the start "
		"fails -ENOMEM, leaving the device unreachable until reboot",
	},
	{
		"fast->slow stop -> recycled",
		{ EV_ADV_STARTED, EV_ADV_STOPPED_FOR_SLOW }, 2, false,
		"field bug #2: stopping the advertiser frees its pre-allocated "
		"conn object and fires recycled too; starting here would race "
		"the slow start and return -EALREADY (spurious error LED)",
	},
	{
		"failed connect -> recycled",
		{ EV_ADV_STARTED, EV_CONNECT_FAILED }, 2, true,
		"on_connected's error path deliberately does not restart; "
		"recycled covers it once the object returns to the pool",
	},
	{
		"connected, a stale retry fires",
		{ EV_ADV_STARTED, EV_CONNECTED }, 2, false,
		"a stale adv_retry_work must no-op while connected",
	},
	{
		"reconnect after a full cycle",
		{ EV_ADV_STARTED, EV_CONNECTED, EV_SECURED, EV_DISCONNECTED }, 4, true,
		"every reconnect must work, not just the first",
	},
};

ZTEST(proxy_core_policy, test_advertising_restart_sequences)
{
	for (size_t i = 0; i < ARRAY_SIZE(adv_sequences); i++) {
		struct proxy_link_state state = { 0 };

		for (size_t e = 0; e < adv_sequences[i].event_count; e++) {
			apply_event(&state, adv_sequences[i].events[e]);
		}

		zassert_equal(proxy_should_start_adv(&state),
			      adv_sequences[i].start_allowed,
			      "%s: advertising_start() must%s proceed -- %s",
			      adv_sequences[i].name,
			      adv_sequences[i].start_allowed ? "" : " not",
			      adv_sequences[i].why);
	}
}

/* The forwarding gate over the same sequences: data must never escape before
 * the link encrypts, and must stop the moment the peer goes away. */
static const struct {
	const char *name;
	enum proxy_event events[MAX_EVENTS];
	size_t event_count;
	bool may_forward;
	const char *why;
} forward_sequences[] = {
	{
		"advertising, nobody connected",
		{ EV_ADV_STARTED }, 1, false,
		"no peer to forward to",
	},
	{
		"connected, not yet encrypted",
		{ EV_ADV_STARTED, EV_CONNECTED }, 2, false,
		"Just Works cannot satisfy BT_NUS_AUTHEN, so this app-level gate "
		"is the pairing lock's actual enforcement",
	},
	{
		"connected and encrypted",
		{ EV_ADV_STARTED, EV_CONNECTED, EV_SECURED }, 3, true,
		"the only state data may flow in",
	},
	{
		"disconnect after a secure session",
		{ EV_ADV_STARTED, EV_CONNECTED, EV_SECURED, EV_DISCONNECTED }, 4, false,
		"link_secure must not survive the peer",
	},
	{
		"a second, unencrypted connection after a secure one",
		{ EV_ADV_STARTED, EV_CONNECTED, EV_SECURED, EV_DISCONNECTED,
		  EV_ADV_STARTED, EV_CONNECTED }, 6, false,
		"the gate must re-arm for every new link",
	},
};

ZTEST(proxy_core_policy, test_forwarding_gate_sequences)
{
	for (size_t i = 0; i < ARRAY_SIZE(forward_sequences); i++) {
		struct proxy_link_state state = { 0 };

		for (size_t e = 0; e < forward_sequences[i].event_count; e++) {
			apply_event(&state, forward_sequences[i].events[e]);
		}

		zassert_equal(proxy_may_forward(&state),
			      forward_sequences[i].may_forward,
			      "%s: must%s forward -- %s", forward_sequences[i].name,
			      forward_sequences[i].may_forward ? "" : " not",
			      forward_sequences[i].why);
	}
}

ZTEST_SUITE(proxy_core_policy, NULL, NULL, NULL, NULL, NULL);
