#include <btest.h>
#include <shibe.h>
#include <shibe/asm.h>
#include <shibe/opcode.h>
#include "common.h"

// The assembler gets an allocator of its own: shibe_asm_end restores it, which
// would take the vm's memory with it if the two were shared
static shibe_barena_t asm_arena;
static shibe_allocator_t* asm_allocator;

static void
asm_init_per_test(void) {
	init_per_test();
	asm_allocator = shibe_barena_init(&asm_arena, &arena_pool);
}

static void
asm_cleanup_per_test(void) {
	barena_reset(&asm_arena.arena);
	// Drop the arena so leak detection works
	asm_arena.arena = (barena_t){ 0 };
	cleanup_per_test();
}

static btest_suite_t sasm = {
	.name = "shibe/asm",

	.init_per_suite = init_per_suite,
	.cleanup_per_suite = cleanup_per_suite,
	.init_per_test = asm_init_per_test,
	.cleanup_per_test = asm_cleanup_per_test,
};

#define CODE_LEN 16u

static inline shibe_cell_t
bundle_of(shibe_opcode_t a, shibe_opcode_t b, shibe_opcode_t c, shibe_opcode_t d) {
	shibe_bundle_t bundle = { a, b, c, d };
	return shibe_pack(bundle);
}

static inline shibe_cell_t
cell_at(shibe_cell_t base, uint32_t index) {
	return shibe_fetch(vm, (shibe_cell_t){ .u32 = base.u32 + index });
}

// Allocate a zeroed run of code memory
static inline shibe_cell_t
alloc_code(void) {
	return shibe_alloc(vm, SHIBE_MEM_REGION_1, (shibe_cell_t){ .u32 = CODE_LEN });
}

BTEST(sasm, bundling) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	// Immediate-taking opcodes share one bundle; their operands follow it in
	// emission order
	shibe_asm_emit_imm(a, SHIBE_OP_LIT, (shibe_cell_t){ .i32 = 42 });
	shibe_asm_emit_imm(a, SHIBE_OP_AGET, (shibe_cell_t){ .u32 = 1 });
	shibe_asm_emit_imm(a, SHIBE_OP_ASET, (shibe_cell_t){ .u32 = 7 });
	shibe_asm_emit(a, SHIBE_OP_ADD);

	BTEST_ASSERT(shibe_asm_end(a));

	BTEST_EXPECT_EQUAL("%u", cell_at(code, 0).u32, bundle_of(
		SHIBE_OP_LIT, SHIBE_OP_AGET, SHIBE_OP_ASET, SHIBE_OP_ADD
	).u32);
	BTEST_EXPECT_EQUAL("%d", cell_at(code, 1).i32, 42);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 2).u32, 1u);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 3).u32, 7u);
	// Nothing past the code was touched
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 4).u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, padding) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	// An explicit align pads with NOP so control falls through it
	shibe_asm_emit(a, SHIBE_OP_ADD);
	shibe_asm_align(a);
	// The trailing bundle is padded with TRAP so running off the end faults
	shibe_asm_emit(a, SHIBE_OP_SUB);

	BTEST_ASSERT(shibe_asm_end(a));

	BTEST_EXPECT_EQUAL("%u", cell_at(code, 0).u32, bundle_of(
		SHIBE_OP_ADD, SHIBE_OP_NOP, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 1).u32, bundle_of(
		SHIBE_OP_SUB, SHIBE_OP_TRAP, SHIBE_OP_TRAP, SHIBE_OP_TRAP
	).u32);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 2).u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, labels) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	shibe_asm_label_t loop = shibe_asm_make_label(a);
	shibe_asm_label_t skip = shibe_asm_make_label(a);

	// Backward reference: bound before it is used
	shibe_asm_bind_label(a, loop);
	BTEST_EXPECT_EQUAL("%u", shibe_asm_here(a).u32, code.u32);

	shibe_asm_emit_imm(a, SHIBE_OP_LIT, (shibe_cell_t){ .u32 = 0 });
	// Forward reference: patched by shibe_asm_end
	shibe_asm_emit_imm_label(a, SHIBE_OP_JZ, skip);
	shibe_asm_emit_imm_label(a, SHIBE_OP_JMP, loop);
	shibe_asm_bind_label(a, skip);
	shibe_asm_emit(a, SHIBE_OP_HALT);

	BTEST_ASSERT(shibe_asm_end(a));

	// JZ ends its bundle, so JMP could not join it
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 0).u32, bundle_of(
		SHIBE_OP_LIT, SHIBE_OP_JZ, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 1).u32, 0u);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 2).u32, code.u32 + 5);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 3).u32, bundle_of(
		SHIBE_OP_JMP, SHIBE_OP_NOP, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 4).u32, code.u32);
	// A bundle closed by a control transfer keeps its NOP padding
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 5).u32, bundle_of(
		SHIBE_OP_HALT, SHIBE_OP_NOP, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, data) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	shibe_asm_label_t target = shibe_asm_make_label(a);

	shibe_asm_emit(a, SHIBE_OP_NOP);
	// Data aligns first, so it never shares a cell with opcodes
	shibe_asm_data(a, (shibe_cell_t){ .u32 = 0xdeadu });
	shibe_asm_data_label(a, target);
	shibe_asm_bind_label(a, target);
	shibe_asm_emit(a, SHIBE_OP_HALT);

	BTEST_ASSERT(shibe_asm_end(a));

	BTEST_EXPECT_EQUAL("%u", cell_at(code, 0).u32, bundle_of(
		SHIBE_OP_NOP, SHIBE_OP_NOP, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 1).u32, 0xdeadu);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 2).u32, code.u32 + 3);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 3).u32, bundle_of(
		SHIBE_OP_HALT, SHIBE_OP_NOP, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, unbound_label_writes_nothing) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	shibe_asm_label_t nowhere = shibe_asm_make_label(a);
	shibe_asm_emit(a, SHIBE_OP_NOP);
	shibe_asm_emit_imm_label(a, SHIBE_OP_JMP, nowhere);

	BTEST_EXPECT(!shibe_asm_end(a));

	// The vm never saw any of it
	for (uint32_t i = 0; i < CODE_LEN; ++i) {
		BTEST_EXPECT_EQUAL("%u", cell_at(code, i).u32, 0u);
	}
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, control_transfer_ends_bundle) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	shibe_asm_label_t skip = shibe_asm_make_label(a);

	shibe_asm_emit(a, SHIBE_OP_ADD);
	shibe_asm_emit_imm_label(a, SHIBE_OP_JZ, skip);
	// Cannot join the bundle JZ is in, however much room is left
	shibe_asm_emit(a, SHIBE_OP_SUB);
	shibe_asm_bind_label(a, skip);
	shibe_asm_emit(a, SHIBE_OP_MUL);

	BTEST_ASSERT(shibe_asm_end(a));

	BTEST_EXPECT_EQUAL("%u", cell_at(code, 0).u32, bundle_of(
		SHIBE_OP_ADD, SHIBE_OP_JZ, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 1).u32, code.u32 + 3);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 2).u32, bundle_of(
		SHIBE_OP_SUB, SHIBE_OP_NOP, SHIBE_OP_NOP, SHIBE_OP_NOP
	).u32);
	// A bundle that no control transfer closed still traps on the way out
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 3).u32, bundle_of(
		SHIBE_OP_MUL, SHIBE_OP_TRAP, SHIBE_OP_TRAP, SHIBE_OP_TRAP
	).u32);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, missing_immediate_writes_nothing) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	// LIT reads the cell after the bundle, so without one the vm would take
	// whatever happens to follow
	shibe_asm_emit(a, SHIBE_OP_LIT);

	BTEST_EXPECT(!shibe_asm_end(a));
	for (uint32_t i = 0; i < CODE_LEN; ++i) {
		BTEST_EXPECT_EQUAL("%u", cell_at(code, i).u32, 0u);
	}
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, unwanted_immediate_writes_nothing) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	// ADD takes nothing from the instruction stream, so the operand cell would
	// be reached and run as a bundle
	shibe_asm_emit_imm(a, SHIBE_OP_ADD, (shibe_cell_t){ .u32 = 1 });

	BTEST_EXPECT(!shibe_asm_end(a));
	for (uint32_t i = 0; i < CODE_LEN; ++i) {
		BTEST_EXPECT_EQUAL("%u", cell_at(code, i).u32, 0u);
	}
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, unwanted_immediate_label_writes_nothing) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	shibe_asm_label_t target = shibe_asm_make_label(a);
	shibe_asm_emit_imm_label(a, SHIBE_OP_ADD, target);
	shibe_asm_bind_label(a, target);

	BTEST_EXPECT(!shibe_asm_end(a));
	for (uint32_t i = 0; i < CODE_LEN; ++i) {
		BTEST_EXPECT_EQUAL("%u", cell_at(code, i).u32, 0u);
	}
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, deferred_value) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	// How many slots a `let` form needs is only known once its body was read
	shibe_asm_label_t num_slots = shibe_asm_make_label(a);
	shibe_asm_emit_imm_label(a, SHIBE_OP_ENTER, num_slots);
	shibe_asm_emit_imm(a, SHIBE_OP_ASET, (shibe_cell_t){ .u32 = 0 });

	// Binding a value does not align, so the bundle keeps filling
	shibe_asm_bind_value(a, num_slots, (shibe_cell_t){ .u32 = 1 });

	shibe_asm_emit(a, SHIBE_OP_LEAVE);
	shibe_asm_emit(a, SHIBE_OP_HALT);

	BTEST_ASSERT(shibe_asm_end(a));

	BTEST_EXPECT_EQUAL("%u", cell_at(code, 0).u32, bundle_of(
		SHIBE_OP_ENTER, SHIBE_OP_ASET, SHIBE_OP_LEAVE, SHIBE_OP_HALT
	).u32);
	// The ENTER operand holds the count that was patched in, not an address
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 1).u32, 1u);
	BTEST_EXPECT_EQUAL("%u", cell_at(code, 2).u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, value_bound_twice_writes_nothing) {
	shibe_cell_t code = alloc_code();
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	shibe_asm_label_t num_slots = shibe_asm_make_label(a);
	shibe_asm_emit_imm_label(a, SHIBE_OP_ENTER, num_slots);
	shibe_asm_bind_value(a, num_slots, (shibe_cell_t){ .u32 = 1 });
	shibe_asm_bind_value(a, num_slots, (shibe_cell_t){ .u32 = 2 });

	BTEST_EXPECT(!shibe_asm_end(a));

	// The vm never saw any of it
	for (uint32_t i = 0; i < CODE_LEN; ++i) {
		BTEST_EXPECT_EQUAL("%u", cell_at(code, i).u32, 0u);
	}
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, end_restores_the_allocator) {
	// Room for more bundles than the first bseg segment holds
	shibe_cell_t code = shibe_alloc(vm, SHIBE_MEM_REGION_3, (shibe_cell_t){ .u32 = 128 });

	void* before = barena_snapshot(&asm_arena.arena);
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	// 80 bundles, enough to spill the code buffer past its first segment
	for (uint32_t i = 0; i < 80u * 4u; ++i) {
		shibe_asm_emit(a, SHIBE_OP_NOP);
	}

	BTEST_ASSERT(shibe_asm_end(a));
	BTEST_EXPECT(barena_snapshot(&asm_arena.arena) == before);
}

// Hands out a fixed number of blocks and then refuses, standing in for the
// embedded case where allocation actually fails. On desktop this path is
// unreachable: the process is killed or swaps rather than seeing NULL.
typedef struct {
	shibe_allocator_t impl;
	int budget;
} starved_allocator_t;

static void*
starved_alloc(shibe_allocator_t* ctx, size_t size, size_t alignment) {
	starved_allocator_t* self = BCONTAINER_OF(ctx, starved_allocator_t, impl);
	if (self->budget <= 0) { return NULL; }

	--self->budget;
	return barena_memalign(&asm_arena.arena, size, alignment);
}

static void*
starved_snapshot(shibe_allocator_t* ctx) {
	(void)ctx;
	return barena_snapshot(&asm_arena.arena);
}

static void
starved_restore(shibe_allocator_t* ctx, void* snapshot) {
	(void)ctx;
	barena_restore(&asm_arena.arena, snapshot);
}

BTEST(sasm, starved_allocator_is_survivable) {
	shibe_cell_t code = alloc_code();
	starved_allocator_t starved = {
		.impl = {
			.alloc = starved_alloc,
			.snapshot = starved_snapshot,
			.restore = starved_restore,
		},
		// Enough for the assembler and one bseg segment, nothing more
		.budget = 2,
	};

	shibe_asm_t* a = shibe_asm_begin(vm, &starved.impl, code);
	BTEST_ASSERT(a != NULL);

	shibe_asm_label_t loop = shibe_asm_make_label(a);
	// Far more than the allocator will back; every one of these has to no-op
	// rather than walk into a segment that was never handed over
	for (uint32_t i = 0; i < 1024u; ++i) {
		shibe_asm_emit(a, SHIBE_OP_NOP);
	}
	shibe_asm_bind_label(a, loop);
	shibe_asm_emit_imm_label(a, SHIBE_OP_JMP, loop);
	shibe_asm_data(a, (shibe_cell_t){ .u32 = 1 });

	BTEST_EXPECT(!shibe_asm_end(a));

	// None of it reached the vm, and it faulted nothing
	for (uint32_t i = 0; i < CODE_LEN; ++i) {
		BTEST_EXPECT_EQUAL("%u", cell_at(code, i).u32, 0u);
	}
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sasm, overrun_faults) {
	// Only two cells of code memory, in a region of its own
	shibe_cell_t code = shibe_alloc(vm, SHIBE_MEM_REGION_2, (shibe_cell_t){ .u32 = 2 });
	shibe_asm_t* a = shibe_asm_begin(vm, asm_allocator, code);
	BTEST_ASSERT(a != NULL);

	// Three bundles will not fit
	for (uint32_t i = 0; i < 3; ++i) {
		shibe_asm_emit(a, SHIBE_OP_NOP);
		shibe_asm_align(a);
	}

	BTEST_EXPECT(!shibe_asm_end(a));
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_MEM_FAULT);
	clear_panic();
}
