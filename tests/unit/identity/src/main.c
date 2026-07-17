/*
 * Group C — per-device identity derivation (ADD_TESTING_PLAN.md).
 *
 * What these protect: without identity_init() every unit advertises the same
 * name and a random-static address regenerated from the RNG on every boot, so
 * units are indistinguishable and the address is useless as an ID. The address
 * is stable across reboots precisely *because* it is recomputed identically
 * rather than stored -- which makes that stability (C6) a property a unit test
 * can check, where on hardware it costs a power cycle and a scanner.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "proxy_core.h"

/* What main.c passes as base_name (CONFIG_BT_DEVICE_NAME). */
#define BASE_NAME "nrfProxy"

/* hwinfo_get_device_id() fills 8 bytes on the nRF52840. Values are arbitrary
 * but must have distinct low/high halves: the address comes from the low 6
 * bytes, the name suffix from bytes 5 and 4, and the mfg id from the low 4, so
 * a mix-up would otherwise pass. */
static const uint8_t hwid[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };

/* An identity with a guard band behind it, so an over-long name shows up as a
 * clobbered guard rather than silent corruption. */
struct guarded_identity {
	struct proxy_identity id;
	uint8_t guard[16];
};

#define GUARD_BYTE 0x5C

static void init_guarded(struct guarded_identity *g)
{
	memset(g, 0, sizeof(*g));
	memset(g->guard, GUARD_BYTE, sizeof(g->guard));
	/* A recognisable prior value, so "left untouched" is provable. */
	memset(g->id.mfg_id, 0xEE, sizeof(g->id.mfg_id));
}

static void assert_guard_intact(const struct guarded_identity *g)
{
	for (size_t i = 0; i < sizeof(g->guard); i++) {
		zassert_equal(g->guard[i], GUARD_BYTE,
			      "wrote past struct proxy_identity at guard byte %u",
			      (unsigned int)i);
	}
}

/* C1 -- the address is the low 6 hwid bytes, marked static-random. */
ZTEST(proxy_core_identity, test_address_is_low_six_bytes_made_static)
{
	struct guarded_identity g;

	init_guarded(&g);
	proxy_identity_derive(BASE_NAME, hwid, sizeof(hwid), &g.id);

	zassert_true(g.id.addr_valid, "an 8-byte hwid must yield an address");

	/* Mirrors BT_ADDR_SET_STATIC: the two MSBs of the top byte are forced.
	 * main.c pairs these bytes with BT_ADDR_LE_RANDOM. */
	const uint8_t want[PROXY_ADDR_LEN] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66 | 0xc0,
	};

	zassert_mem_equal(g.id.addr, want, sizeof(want),
			  "address is not the low 6 hwid bytes made static-random");
	zassert_equal(g.id.addr[PROXY_ADDR_LEN - 1] & 0xc0, 0xc0,
		      "top two bits must be set or the controller rejects the address");
	assert_guard_intact(&g);
}

/* C2 -- the name suffix is hwid[5] then hwid[4], uppercase hex. */
ZTEST(proxy_core_identity, test_name_suffix_format)
{
	struct guarded_identity g;

	init_guarded(&g);
	proxy_identity_derive(BASE_NAME, hwid, sizeof(hwid), &g.id);

	zassert_str_equal(g.id.name, BASE_NAME "-6655",
			  "name is \"%s\"", g.id.name);
	assert_guard_intact(&g);
}

/* C4 -- the manufacturer-AD id is the low 4 hwid bytes. */
ZTEST(proxy_core_identity, test_mfg_id_is_low_four_bytes)
{
	struct guarded_identity g;

	init_guarded(&g);
	proxy_identity_derive(BASE_NAME, hwid, sizeof(hwid), &g.id);

	zassert_mem_equal(g.id.mfg_id, hwid, PROXY_MFG_ID_LEN,
			  "mfg id is not the low 4 hwid bytes");
}

/* C3 -- a base name filling the buffer neither overflows nor loses its NUL.
 * The suffix is dropped rather than truncating the name or running past the
 * end; main.c BUILD_ASSERTs that the real name always leaves room for it. */
ZTEST(proxy_core_identity, test_max_length_base_name_is_safe)
{
	struct guarded_identity g;
	char long_name[PROXY_DEVICE_NAME_MAX];

	memset(long_name, 'X', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';

	init_guarded(&g);
	proxy_identity_derive(long_name, hwid, sizeof(hwid), &g.id);

	zassert_equal(g.id.name[PROXY_DEVICE_NAME_MAX - 1], '\0',
		      "name must always be NUL-terminated");
	zassert_str_equal(g.id.name, long_name, "base name must survive whole");
	assert_guard_intact(&g);
}

/* An over-long base name truncates, and still terminates. */
ZTEST(proxy_core_identity, test_over_long_base_name_truncates_safely)
{
	struct guarded_identity g;
	char long_name[PROXY_DEVICE_NAME_MAX + 32];

	memset(long_name, 'Y', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';

	init_guarded(&g);
	proxy_identity_derive(long_name, hwid, sizeof(hwid), &g.id);

	zassert_equal(strlen(g.id.name), PROXY_DEVICE_NAME_MAX - 1,
		      "name must be truncated to the buffer, not overflow it");
	assert_guard_intact(&g);
}

/* C5 -- too short, empty, or an hwinfo error: fall back, touch nothing else. */
ZTEST(proxy_core_identity, test_short_or_failed_hwid_falls_back)
{
	/* hwinfo_get_device_id() returns a negative errno on failure, so the
	 * length is signed and -EIO must land in the same branch as 0 and 5. */
	const int lengths[] = { 0, 5, -5, -1 };

	for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) {
		struct guarded_identity g;

		init_guarded(&g);
		proxy_identity_derive(BASE_NAME, hwid, lengths[i], &g.id);

		zassert_false(g.id.addr_valid, "hwid_len %d must not yield an address",
			      lengths[i]);
		zassert_str_equal(g.id.name, BASE_NAME,
				  "hwid_len %d must fall back to the base name",
				  lengths[i]);

		/* "mfg untouched": main.c leaves the compile-time AD bytes in
		 * place, so the field must not be zeroed either. */
		for (size_t b = 0; b < PROXY_MFG_ID_LEN; b++) {
			zassert_equal(g.id.mfg_id[b], 0xEE,
				      "hwid_len %d clobbered the mfg id",
				      lengths[i]);
		}
		assert_guard_intact(&g);
	}
}

/* A NULL hwid is the same fallback, not a crash. */
ZTEST(proxy_core_identity, test_null_hwid_falls_back)
{
	struct guarded_identity g;

	init_guarded(&g);
	proxy_identity_derive(BASE_NAME, NULL, 8, &g.id);

	zassert_false(g.id.addr_valid, "a NULL hwid must not yield an address");
	zassert_str_equal(g.id.name, BASE_NAME, "must fall back to the base name");
}

/* Exactly the minimum length works: the address needs 6 bytes and no more. */
ZTEST(proxy_core_identity, test_minimum_length_hwid_is_accepted)
{
	struct guarded_identity g;

	init_guarded(&g);
	proxy_identity_derive(BASE_NAME, hwid, PROXY_HWID_MIN_LEN, &g.id);

	zassert_true(g.id.addr_valid, "a 6-byte hwid is enough for an address");
	zassert_str_equal(g.id.name, BASE_NAME "-6655", "name is \"%s\"", g.id.name);
}

/* C6 -- determinism. This is the reboot-stability property: same chip, same
 * identity, every boot, with nothing persisted to flash. */
ZTEST(proxy_core_identity, test_derivation_is_deterministic)
{
	struct proxy_identity first;
	struct proxy_identity second;

	/* Opposite starting fills, so any field the derivation fails to write
	 * shows up as a difference rather than as two matching zeroes. */
	memset(&first, 0, sizeof(first));
	memset(&second, 0xFF, sizeof(second));

	proxy_identity_derive(BASE_NAME, hwid, sizeof(hwid), &first);
	proxy_identity_derive(BASE_NAME, hwid, sizeof(hwid), &second);

	/* Field by field rather than one memcmp over the struct: struct padding
	 * is never written by anyone, so a whole-struct compare would be a
	 * layout tripwire, not a determinism check. The name is compared over
	 * its whole buffer (not strcmp) because the padding past the NUL is part
	 * of the deterministic output too. */
	zassert_equal(first.addr_valid, second.addr_valid,
		      "addr_valid must not depend on the caller's memory");
	zassert_mem_equal(first.addr, second.addr, PROXY_ADDR_LEN,
			  "the same hwid must derive the same address every boot");
	zassert_mem_equal(first.name, second.name, sizeof(first.name),
			  "the same hwid must derive the same name every boot");
	zassert_mem_equal(first.mfg_id, second.mfg_id, PROXY_MFG_ID_LEN,
			  "the same hwid must derive the same advertised id every boot");
}

/* Different chips must not collide in the address or the id (the 16-bit name
 * suffix can collide -- that is known and documented). */
ZTEST(proxy_core_identity, test_different_hwids_differ)
{
	const uint8_t other[8] = { 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22 };
	struct proxy_identity a;
	struct proxy_identity b;

	proxy_identity_derive(BASE_NAME, hwid, sizeof(hwid), &a);
	proxy_identity_derive(BASE_NAME, other, sizeof(other), &b);

	zassert_true(memcmp(a.addr, b.addr, PROXY_ADDR_LEN) != 0,
		     "different chips must get different addresses");
	zassert_true(memcmp(a.mfg_id, b.mfg_id, PROXY_MFG_ID_LEN) != 0,
		     "different chips must get different advertised ids");
}

ZTEST_SUITE(proxy_core_identity, NULL, NULL, NULL, NULL, NULL);
