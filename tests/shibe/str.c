#include <btest.h>
#include <shibe.h>
#include <shibe/str.h>
#include <string.h>
#include "common.h"

static btest_suite_t str = {
	.name = "shibe/str",

	.init_per_suite = init_per_suite,
	.cleanup_per_suite = cleanup_per_suite,
	.init_per_test = init_per_test,
	.cleanup_per_test = cleanup_per_test,
};

// Read the payload back as raw bytes, terminator included
static void
read_payload(shibe_cell_t addr, char* buf, uint32_t num_bytes) {
	shibe_copy_to_host(vm, shibe_str_payload(addr), buf, num_bytes);
}

BTEST(str, layout) {
	// A string that does not fill its last cell
	BTEST_EXPECT_EQUAL("%u", shibe_str_num_cells(0), 2u);
	BTEST_EXPECT_EQUAL("%u", shibe_str_num_cells(3), 2u);
	// The terminator spills into another cell when the bytes fill one exactly
	BTEST_EXPECT_EQUAL("%u", shibe_str_num_cells(4), 3u);
	BTEST_EXPECT_EQUAL("%u", shibe_str_num_cells(7), 3u);
	BTEST_EXPECT_EQUAL("%u", shibe_str_num_cells(8), 4u);

	const char hello[] = "hello";
	shibe_cell_t addr = shibe_str_alloc(vm, SHIBE_MEM_REGION_1, hello, 5);
	BTEST_ASSERT_EQUAL("%d", num_panics, 0);
	// Strings start on a cell boundary, so the address is a plain cell index
	BTEST_ASSERT(shibe_mem_region(addr) == SHIBE_MEM_REGION_1);

	// The length cell does not count the terminator
	BTEST_EXPECT_EQUAL("%u", shibe_str_len(vm, addr), 5u);

	// The payload is terminated and the rest of the last cell is zero padded
	char payload[8] = { 0 };
	read_payload(addr, payload, 8);
	BTEST_EXPECT(memcmp(payload, "hello\0\0\0", 8) == 0);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);

	// The next string starts right after, still on a cell boundary
	shibe_cell_t next = shibe_str_alloc(vm, SHIBE_MEM_REGION_1, "hi", 2);
	BTEST_EXPECT_EQUAL("%u", next.u32, addr.u32 + shibe_str_num_cells(5));
	BTEST_EXPECT_EQUAL("%u", shibe_str_len(vm, next), 2u);
}

BTEST(str, round_trip) {
	// Lengths either side of a cell boundary, and the empty string
	static const char* const cases[] = { "", "a", "abc", "abcd", "abcde", "0123456789" };

	for (uint32_t i = 0; i < BCOUNT_OF(cases); ++i) {
		const char* text = cases[i];
		uint32_t len = (uint32_t)strlen(text);

		shibe_cell_t addr = shibe_str_alloc(vm, SHIBE_MEM_REGION_2, text, len);
		BTEST_ASSERT_EQUAL("%d", num_panics, 0);
		BTEST_EXPECT_EQUAL("%u", shibe_str_len(vm, addr), len);

		char buf[16] = { 0 };
		uint32_t reported = shibe_str_copy(vm, addr, buf, sizeof(buf));
		BTEST_EXPECT_EQUAL("%u", reported, len);
		BTEST_EXPECT_EX(strcmp(buf, text) == 0, "got \"%s\"", buf);
	}

	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(str, copy_truncates) {
	shibe_cell_t addr = shibe_str_alloc(vm, SHIBE_MEM_REGION_3, "hello", 5);
	BTEST_ASSERT_EQUAL("%d", num_panics, 0);

	// A short buffer is filled and terminated, the full length is still reported
	char buf[4] = { 'x', 'x', 'x', 'x' };
	BTEST_EXPECT_EQUAL("%u", shibe_str_copy(vm, addr, buf, sizeof(buf)), 5u);
	BTEST_EXPECT_EX(strcmp(buf, "hel") == 0, "got \"%s\"", buf);

	// A zero sized buffer is left alone
	char untouched = 'x';
	BTEST_EXPECT_EQUAL("%u", shibe_str_copy(vm, addr, &untouched, 0), 5u);
	BTEST_EXPECT_EQUAL("%d", untouched, 'x');

	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(str, bad_address) {
	// Reading a string outside of any allocation faults instead of returning junk
	shibe_cell_t unmapped = { .u32 = SHIBE_MEM_REGION_7 << 29 };
	BTEST_EXPECT_EQUAL("%u", shibe_str_len(vm, unmapped), 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	clear_panic();

	char buf[8] = { 0 };
	BTEST_EXPECT_EQUAL("%u", shibe_str_copy(vm, unmapped, buf, sizeof(buf)), 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	clear_panic();
}
