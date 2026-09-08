#include <btest.h>
#include <shibe.h>
#include "common.h"

static btest_suite_t basic = {
	.name = "shibe/basic",

	.init_per_suite = init_per_suite,
	.cleanup_per_suite = cleanup_per_suite,
	.init_per_test = init_per_test,
	.cleanup_per_test = cleanup_per_test,
};

static inline shibe_cell_t
offset(shibe_cell_t addr, uint32_t num_cells) {
	return (shibe_cell_t){ .u32 = addr.u32 + num_cells };
}

BTEST(basic, memory) {
	shibe_cell_t cells = shibe_alloc(vm, SHIBE_MEM_REGION_0, (shibe_cell_t){ 5 });
	BTEST_ASSERT(cells.u32 != 0);
	BTEST_EXPECT(shibe_mem_region(cells) == SHIBE_MEM_REGION_0);

	// Fresh memory is zeroed
	for (uint32_t i = 0; i < 5; ++i) {
		shibe_cell_t cell = shibe_fetch(vm, offset(cells, i));
		BTEST_EXPECT(cell.u32 == 0);
	}

	// Store then fetch
	for (uint32_t i = 0; i < 5; ++i) {
		shibe_store(vm, offset(cells, i), (shibe_cell_t){ .u32 = i + 1 });
	}
	for (uint32_t i = 0; i < 5; ++i) {
		shibe_cell_t cell = shibe_fetch(vm, offset(cells, i));
		BTEST_EXPECT_EQUAL("%d", cell.i32, (int32_t)(i + 1));
	}

	// Bulk transfer in both directions
	shibe_cell_t to_vm[] = {
		{ .i32 = -1 }, { .i32 = -2 }, { .i32 = -3 }, { .i32 = -4 }, { .i32 = -5 },
	};
	shibe_copy_to_vm(vm, cells, to_vm, BCOUNT_OF(to_vm));

	shibe_cell_t from_vm[BCOUNT_OF(to_vm)] = { 0 };
	shibe_copy_to_host(vm, cells, from_vm, BCOUNT_OF(from_vm));
	for (uint32_t i = 0; i < BCOUNT_OF(to_vm); ++i) {
		BTEST_EXPECT_EQUAL("%d", from_vm[i].i32, to_vm[i].i32);
	}

	// Regions are separate address spaces
	shibe_cell_t other = shibe_alloc(vm, SHIBE_MEM_REGION_3, (shibe_cell_t){ 5 });
	BTEST_ASSERT(shibe_mem_region(other) == SHIBE_MEM_REGION_3);
	BTEST_EXPECT(shibe_fetch(vm, other).u32 == 0);
	shibe_store(vm, other, (shibe_cell_t){ .u32 = 42 });
	BTEST_EXPECT_EQUAL("%d", shibe_fetch(vm, other).i32, 42);
	BTEST_EXPECT_EQUAL("%d", shibe_fetch(vm, cells).i32, -1);

	// None of the above is a fault
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
}

BTEST(basic, memory_out_of_bound) {
	shibe_cell_t cells = shibe_alloc(vm, SHIBE_MEM_REGION_0, (shibe_cell_t){ 4 });
	BTEST_ASSERT(cells.u32 != 0);
	shibe_cell_t past_end = offset(cells, 4);

	// Fetch past the end of the region
	shibe_cell_t cell = shibe_fetch(vm, past_end);
	BTEST_EXPECT(cell.u32 == 0);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, past_end.u32);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
	clear_panic();
	BTEST_ASSERT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);

	// Store past the end of the region
	shibe_store(vm, past_end, (shibe_cell_t){ .u32 = 1 });
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, past_end.u32);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
	clear_panic();

	// An untouched region has no valid address at all
	shibe_cell_t unmapped = { .u32 = SHIBE_MEM_REGION_7 << 29 };
	shibe_fetch(vm, unmapped);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, unmapped.u32);
	clear_panic();

	// A bulk write straddling the end faults and stops at the boundary
	shibe_cell_t to_vm[] = {
		{ .i32 = 11 }, { .i32 = 22 }, { .i32 = 33 }, { .i32 = 44 }, { .i32 = 55 },
	};
	shibe_copy_to_vm(vm, offset(cells, 2), to_vm, BCOUNT_OF(to_vm));
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, past_end.u32);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
	clear_panic();

	// The in-bound prefix was written, the rest never touched vm memory
	BTEST_EXPECT_EQUAL("%d", shibe_fetch(vm, offset(cells, 2)).i32, 11);
	BTEST_EXPECT_EQUAL("%d", shibe_fetch(vm, offset(cells, 3)).i32, 22);
	BTEST_ASSERT_EQUAL("%d", num_panics, 0);

	// Same for a bulk read
	shibe_cell_t from_vm[BCOUNT_OF(to_vm)] = {
		{ .i32 = -1 }, { .i32 = -1 }, { .i32 = -1 }, { .i32 = -1 }, { .i32 = -1 },
	};
	shibe_copy_to_host(vm, offset(cells, 2), from_vm, BCOUNT_OF(from_vm));
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, past_end.u32);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
	clear_panic();

	BTEST_EXPECT_EQUAL("%d", from_vm[0].i32, 11);
	BTEST_EXPECT_EQUAL("%d", from_vm[1].i32, 22);
	// The host buffer is left alone from the faulting cell onwards
	for (uint32_t i = 2; i < BCOUNT_OF(from_vm); ++i) {
		BTEST_EXPECT_EQUAL("%d", from_vm[i].i32, -1);
	}

	// An empty copy at an invalid address touches nothing and does not fault
	shibe_copy_to_vm(vm, unmapped, to_vm, 0);
	shibe_copy_to_host(vm, unmapped, from_vm, 0);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
}

BTEST(basic, stack) {
	const shibe_state_t* state = shibe_inspect(vm);
	BTEST_ASSERT_EQUAL("%u", state->dsp.u32, 0u);

	// Push then pop in LIFO order
	shibe_cell_t items[] = {
		{ .i32 = 11 }, { .i32 = -22 }, { .f32 = 3.5f },
	};
	for (uint32_t i = 0; i < BCOUNT_OF(items); ++i) {
		shibe_push(vm, items[i]);
		BTEST_EXPECT_EQUAL("%u", state->dsp.u32, i + 1);
	}

	for (uint32_t i = BCOUNT_OF(items); i-- > 0;) {
		shibe_cell_t item = shibe_pop(vm);
		BTEST_EXPECT_EQUAL("%u", item.u32, items[i].u32);
		BTEST_EXPECT_EQUAL("%u", state->dsp.u32, i);
	}

	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(state->exec_state == SHIBE_EXEC_IDLE);

	// Popping an empty stack faults
	shibe_cell_t item = shibe_pop(vm);
	BTEST_EXPECT_EQUAL("%u", item.u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_STACK_UNDERFLOW);
	BTEST_EXPECT(state->exec_state == SHIBE_EXEC_PANIC);
	BTEST_EXPECT_EQUAL("%u", state->dsp.u32, 0u);
	clear_panic();

	// The stack is usable again after the panic is cleared
	shibe_push(vm, (shibe_cell_t){ .i32 = 7 });
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 7);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(basic, stack_overflow) {
	const shibe_state_t* state = shibe_inspect(vm);
	for (uint32_t i = 0; i < TEST_DS_LEN; ++i) {
		shibe_push(vm, (shibe_cell_t){ .u32 = i + 1 });
	}
	BTEST_ASSERT_EQUAL("%d", num_panics, 0);
	BTEST_ASSERT_EQUAL("%u", state->dsp.u32, TEST_DS_LEN);

	// One past the end faults and leaves the stack untouched
	shibe_push(vm, (shibe_cell_t){ .u32 = 0xdeadbeef });
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_STACK_OVERFLOW);
	BTEST_EXPECT(state->exec_state == SHIBE_EXEC_PANIC);
	BTEST_EXPECT_EQUAL("%u", state->dsp.u32, TEST_DS_LEN);
	for (uint32_t i = 0; i < TEST_DS_LEN; ++i) {
		BTEST_EXPECT_EQUAL("%u", state->ds[i].u32, i + 1);
	}
}
