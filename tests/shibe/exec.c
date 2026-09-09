#include <btest.h>
#include <shibe.h>
#include <shibe/asm.h>
#include <shibe/opcode.h>
#include "common.h"

// The assembler gets an allocator of its own: shibe_asm_end restores it
static shibe_barena_t exec_arena;
static shibe_allocator_t* exec_allocator;

#define CODE_LEN 64u

static shibe_cell_t code;
static shibe_asm_t* sasm;

// Recorded by the hooks below
#define MAX_STEPS 32
static shibe_op_addr_t steps[MAX_STEPS];
static int num_steps;
static shibe_cell_t last_extcall;
static int num_extcalls;

static shibe_status_t
record_step(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)hooked; (void)state;
	if (num_steps < MAX_STEPS) { steps[num_steps] = at; }
	++num_steps;
	return SHIBE_OK;
}

static shibe_status_t
record_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	last_extcall = index;
	++num_extcalls;
	shibe_push(called, (shibe_cell_t){ .i32 = 99 });
	return SHIBE_OK;
}

// Fails on its own, without touching the vm
static shibe_status_t
failing_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)called; (void)index;
	++num_extcalls;
	return SHIBE_ERROR;
}

// Panics the vm from underneath the interpreter, the way a nested
// shibe_execute or any faulting api call would, then relays the failure
static shibe_status_t
relaying_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	shibe_pop(called);
	return SHIBE_ERROR;
}

static shibe_status_t
suspending_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)called; (void)index;
	++num_extcalls;
	return SHIBE_SUSPENDED;
}

// Suspends on every single opcode, so a resume that reported the opcode it
// picks up on all over again would never get anywhere
static shibe_status_t
single_stepping_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)hooked; (void)state;
	if (num_steps < MAX_STEPS) { steps[num_steps] = at; }
	++num_steps;
	return SHIBE_SUSPENDED;
}

// Suspends on one nominated opcode, named by where it sits rather than by a
// step count so that it can pick one out of a nested run
static shibe_op_addr_t suspend_at;

static shibe_status_t
suspending_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)hooked; (void)state;
	if (num_steps < MAX_STEPS) { steps[num_steps] = at; }
	++num_steps;
	return at.bundle.u32 == suspend_at.bundle.u32 && at.slot == suspend_at.slot
		? SHIBE_SUSPENDED
		: SHIBE_OK;
}

static shibe_status_t
failing_hook(shibe_host_t* host, shibe_vm_t* hooked, const shibe_state_t* state, shibe_op_addr_t at) {
	(void)host; (void)hooked; (void)state; (void)at;
	++num_steps;
	return SHIBE_ERROR;
}

// The stub a compiler would emit for an exported word, re-entered by the
// callback below
static shibe_cell_t nested_entry;

static shibe_status_t
reentrant_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	return shibe_execute(called, nested_entry);
}

// Reads the host boundary out of the auxiliary stack from inside a nested run
static shibe_cell_t observed_creator;
static shibe_cell_t observed_outer_ip;
static shibe_cell_t observed_saved_fp;

static shibe_status_t
inspecting_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;

	const shibe_state_t* st = shibe_inspect(called);
	uint32_t fp = st->fp.u32;
	// creator sits at fp-1 and the outer run's resume point at the bottom of the
	// seven cell host header. Read raw rather than through shibe_get_local: this
	// call has no frame of its own, it is walking somebody else's.
	observed_creator = st->as[fp - 1];
	observed_saved_fp = st->as[fp - 3];
	observed_outer_ip = st->as[fp - 7];
	return SHIBE_OK;
}

// Call 1 re-enters, call 2 reads the boundary the re-entry left behind
static shibe_status_t
boundary_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	return index.u32 == 1
		? reentrant_extcall(host, called, index)
		: inspecting_extcall(host, called, index);
}

// Call 1 re-enters, and the nested run it starts is the one that suspends
static shibe_status_t
nested_suspending_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	return index.u32 == 1
		? reentrant_extcall(host, called, index)
		: suspending_extcall(host, called, index);
}

// Re-enters, lets the nested run finish, then suspends. The frame the re-entry
// opened is still this call's, so the suspension needs a continuation to finish
// it - which is call 9 below, standing in for the rest of this function.
static shibe_status_t
reenter_then_suspend_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	// Call 9 is the rest of this function, reached after the resume
	if (index.u32 == 9) {
		shibe_push(called, (shibe_cell_t){ .i32 = 5 });
		return SHIBE_OK;
	}

	shibe_frame_t frame = shibe_alloc_frame(called, 0);
	shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 9 });
	shibe_status_t status = shibe_execute(called, nested_entry);
	return status == SHIBE_OK ? SHIBE_SUSPENDED : status;
}

// Read back out of a host frame by the callbacks below
static shibe_cell_t observed_slot;
static shibe_cell_t observed_frame_head;
static int num_continuations;

// Call 1 re-enters with a continuation and a couple of context slots, call 2
// suspends the nested run, call 3 is the second half of call 1
static shibe_status_t
continuing_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	switch (index.u32) {
		case 1: {
			shibe_frame_t frame = shibe_alloc_frame(called, 2);
			shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 3 });
			shibe_status_t status = shibe_execute(called, nested_entry);
			// The frame is still this call's on the way out, so what the second
			// half needs goes in now, with the C locals still in scope
			if (status == SHIBE_SUSPENDED) {
				shibe_set_local(called, frame, 0, (shibe_cell_t){ .i32 = 42 });
			}
			return status;
		}
		case 2:
			return SHIBE_SUSPENDED;
		default: {
			++num_continuations;
			// Reached through call 1's frame, so its locals are right there
			shibe_frame_t frame = shibe_get_frame(called);
			observed_slot = shibe_get_local(called, frame, 0);
			shibe_push(called, (shibe_cell_t){ .i32 = 7 });
			return SHIBE_OK;
		}
	}
}

// As above, but the second half is not ready the first two times it is asked
static shibe_status_t
retrying_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	switch (index.u32) {
		case 1: {
			shibe_frame_t frame = shibe_alloc_frame(called, 0);
			shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 3 });
			return shibe_execute(called, nested_entry);
		}
		case 2:
			return SHIBE_SUSPENDED;
		default:
			if (++num_continuations < 3) { return SHIBE_SUSPENDED; }
			shibe_push(called, (shibe_cell_t){ .i32 = 7 });
			return SHIBE_OK;
	}
}

// A leaf call that never re-enters, describing itself for a stack walker
static shibe_status_t
annotating_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;

	shibe_frame_t frame = shibe_alloc_frame(called, 2);
	shibe_set_local(called, frame, 0, (shibe_cell_t){ .u32 = 0xf11eu });
	shibe_set_local(called, frame, 1, (shibe_cell_t){ .u32 = 123u });

	// What a walker would find: a host frame, and the call's own note in it
	const shibe_state_t* st = shibe_inspect(called);
	observed_frame_head = st->as[st->fp.u32 - 1];
	observed_slot = st->as[st->fp.u32 + 1];
	return SHIBE_OK;
}

static uint32_t frame_locals;
static uint32_t bad_local;

// Indexes outside the frame it asked for, which is a host bug rather than
// something to read as zero
static shibe_status_t
bad_local_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	shibe_frame_t frame = shibe_alloc_frame(called, frame_locals);
	observed_slot = shibe_get_local(called, frame, bad_local);
	return SHIBE_OK;
}

// Call 1 stops the run; call 5 is the second half of the host function that
// started it from outside the vm
static shibe_status_t
top_level_continuation_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	if (index.u32 == 5) {
		++num_continuations;
		observed_slot = shibe_get_local(called, shibe_get_frame(called), 0);
		return SHIBE_OK;
	}

	return SHIBE_SUSPENDED;
}

// Reaches for a frame the call never asked for
static shibe_status_t
frameless_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	shibe_get_frame(called);
	return SHIBE_OK;
}

// The plain async case: a leaf call that never re-enters, suspends, and is
// finished later by call 4. The frame is where it keeps both the continuation
// and the context that second half needs.
static shibe_status_t
deferring_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host;
	++num_extcalls;

	if (index.u32 == 4) {
		++num_continuations;
		shibe_push(called, shibe_get_local(called, shibe_get_frame(called), 0));
		return SHIBE_OK;
	}

	shibe_frame_t frame = shibe_alloc_frame(called, 1);
	shibe_set_local(called, frame, 0, (shibe_cell_t){ .i32 = 7 });
	shibe_set_continuation(called, frame, (shibe_cell_t){ .u32 = 4 });
	return SHIBE_SUSPENDED;
}

// Suspends holding a frame nobody could ever finish
static shibe_status_t
stranding_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	shibe_alloc_frame(called, 1);
	return SHIBE_SUSPENDED;
}

// Asks twice, which grows the one frame rather than stacking another
static shibe_status_t
regrowing_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;

	shibe_frame_t first = shibe_alloc_frame(called, 1);
	shibe_set_local(called, first, 0, (shibe_cell_t){ .u32 = 11u });

	shibe_frame_t second = shibe_alloc_frame(called, 3);
	observed_frame_head = (shibe_cell_t){ .u32 = first.fp == second.fp };
	// Cells that were already there keep what was in them
	observed_slot = shibe_get_local(called, second, 0);
	shibe_set_local(called, second, 2, (shibe_cell_t){ .u32 = 33u });
	return SHIBE_OK;
}

// Re-enters over and over without ever completing, to exhaust the aux stack
static shibe_cell_t recursive_entry;

static shibe_status_t
recursing_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	return shibe_execute(called, recursive_entry);
}

static void
exec_init_per_test(void) {
	init_per_test();
	exec_allocator = shibe_barena_init(&exec_arena, &arena_pool);
	test_host.debug = NULL;
	test_host.extcall = NULL;
	num_steps = 0;
	num_extcalls = 0;
	observed_creator = (shibe_cell_t){ .u32 = 0xffffffffu };
	observed_outer_ip = (shibe_cell_t){ 0 };
	observed_saved_fp = (shibe_cell_t){ 0 };
	last_extcall = (shibe_cell_t){ 0 };
	// No real opcode sits at address 0, so nothing matches until a test says so
	suspend_at = (shibe_op_addr_t){ 0 };
	observed_slot = (shibe_cell_t){ .u32 = 0xffffffffu };
	observed_frame_head = (shibe_cell_t){ .u32 = 0xffffffffu };
	num_continuations = 0;
	frame_locals = 0;
	bad_local = 0;

	code = shibe_alloc(vm, SHIBE_MEM_REGION_1, (shibe_cell_t){ .u32 = CODE_LEN });
	sasm = shibe_asm_begin(vm, exec_allocator, code);
}

static void
exec_cleanup_per_test(void) {
	barena_reset(&exec_arena.arena);
	exec_arena.arena = (barena_t){ 0 };
	test_host.debug = NULL;
	test_host.extcall = NULL;
	cleanup_per_test();
}

static btest_suite_t sexec = {
	.name = "shibe/exec",

	.init_per_suite = init_per_suite,
	.cleanup_per_suite = cleanup_per_suite,
	.init_per_test = exec_init_per_test,
	.cleanup_per_test = exec_cleanup_per_test,
};

#define EMIT(OPCODE)          shibe_asm_emit(sasm, SHIBE_OP_ ## OPCODE)
#define EMIT_IMM(OPCODE, V)   shibe_asm_emit_imm(sasm, SHIBE_OP_ ## OPCODE, (V))
#define EMIT_LABEL(OPCODE, L) shibe_asm_emit_imm_label(sasm, SHIBE_OP_ ## OPCODE, (L))
#define LIT(V)                EMIT_IMM(LIT, ((shibe_cell_t){ .i32 = (V) }))
#define LITU(V)               EMIT_IMM(LIT, ((shibe_cell_t){ .u32 = (V) }))

// Assembles what has been emitted and runs it from the top
static inline shibe_status_t
run(void) {
	if (!shibe_asm_end(sasm)) { return SHIBE_ERROR; }
	return shibe_execute(vm, code);
}

static inline uint32_t
depth(void) {
	return shibe_inspect(vm)->dsp.u32;
}

BTEST(sexec, arithmetic) {
	LIT(2);
	LIT(3);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 5);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, runs_across_bundles) {
	// Nine opcodes and four operand cells, so the window is refilled repeatedly
	LIT(1); LIT(2); LIT(3); LIT(4);
	EMIT(ADD); EMIT(ADD); EMIT(ADD);
	EMIT(DUP);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", depth(), 2u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 10);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 10);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, stack_shuffling) {
	LIT(1); LIT(2); LIT(3);
	// a b c -- b c a
	EMIT(ROT);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 1);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 2);
}

static int32_t
run_branch(int32_t cond) {
	shibe_asm_label_t otherwise = shibe_asm_make_label(sasm);
	shibe_asm_label_t done = shibe_asm_make_label(sasm);

	LIT(cond);
	EMIT_LABEL(JZ, otherwise);
	LIT(111);
	EMIT_LABEL(JMP, done);
	shibe_asm_bind_label(sasm, otherwise);
	LIT(222);
	shibe_asm_bind_label(sasm, done);
	EMIT(HALT);

	if (run() != SHIBE_OK) { return -1; }
	return shibe_pop(vm).i32;
}

BTEST(sexec, branch_not_taken) {
	BTEST_EXPECT_EQUAL("%d", run_branch(1), 111);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, branch_taken) {
	BTEST_EXPECT_EQUAL("%d", run_branch(0), 222);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, call_and_ret) {
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);

	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	// CALL ends its bundle, so this is where the return address points
	LIT(1);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	LIT(41);
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 42);
	// The return address was popped again
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, aux_frame_round_trip) {
	EMIT_IMM(ENTER, ((shibe_cell_t){ .u32 = 2 }));
	LIT(7);
	EMIT_IMM(ASET, ((shibe_cell_t){ .i32 = 0 }));
	LIT(8);
	EMIT_IMM(ASET, ((shibe_cell_t){ .i32 = 1 }));
	EMIT_IMM(AGET, ((shibe_cell_t){ .i32 = 1 }));
	EMIT_IMM(AGET, ((shibe_cell_t){ .i32 = 0 }));
	EMIT(LEAVE);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 7);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 8);
	// LEAVE unwound the frame but left the data stack alone
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->fp.u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, frame_header_is_readable_and_not_writable) {
	EMIT_IMM(ENTER, ((shibe_cell_t){ .u32 = 1 }));
	// A negative slot is dropped rather than faulting, so the header survives
	LIT(1234);
	EMIT_IMM(ASET, ((shibe_cell_t){ .i32 = -1 }));
	EMIT_IMM(AGET, ((shibe_cell_t){ .i32 = -1 }));
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	// creator is the address of the ENTER operand cell, which is the cell right
	// after the first bundle
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, code.u32 + 1u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, temporary_arena_nests) {
	// Without ENTER reseeding tm from tp, the inner LEAVE would drop tp all the
	// way back to the outermost mark and free the caller's allocations
	LIT(10); EMIT(TSET);
	EMIT_IMM(ENTER, ((shibe_cell_t){ .u32 = 0 }));
	LIT(20); EMIT(TSET);
	EMIT_IMM(ENTER, ((shibe_cell_t){ .u32 = 0 }));
	LIT(30); EMIT(TSET);
	EMIT(LEAVE);
	EMIT(TGET);
	EMIT(LEAVE);
	EMIT(TGET);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 10u);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 20u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, retained_allocation_survives_leave) {
	LIT(10); EMIT(TSET);
	EMIT_IMM(ENTER, ((shibe_cell_t){ .u32 = 0 }));
	LIT(30); EMIT(TSET);
	// Move the mark over the allocation so LEAVE does not roll it back
	LIT(30); EMIT(TMARK);
	EMIT(LEAVE);
	EMIT(TGET);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 30u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, unwind_restores_the_data_stack) {
	LIT(5);
	EMIT_IMM(ENTER, ((shibe_cell_t){ .u32 = 0 }));
	LIT(6); LIT(7);
	EMIT(UNWIND);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 5);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, memory_round_trip) {
	shibe_cell_t data = shibe_alloc(vm, SHIBE_MEM_REGION_2, (shibe_cell_t){ .u32 = 4 });

	LIT(0x11223344);
	LITU(data.u32);
	EMIT(STORE);
	LITU(data.u32);
	EMIT(FETCH);
	// Byte 1 of the cell, little endian
	LITU(data.u32);
	LITU(1);
	EMIT(BFETCH);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 0x33u);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 0x11223344u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, byte_store_masks_rather_than_faults) {
	shibe_cell_t data = shibe_alloc(vm, SHIBE_MEM_REGION_2, (shibe_cell_t){ .u32 = 1 });

	// value 0x1ff masks to 0xff, offset 5 masks to 1
	LITU(0x1ff);
	LITU(data.u32);
	LITU(5);
	EMIT(BSTORE);
	LITU(data.u32);
	EMIT(FETCH);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 0x0000ff00u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, bulk_copy) {
	shibe_cell_t data = shibe_alloc(vm, SHIBE_MEM_REGION_2, (shibe_cell_t){ .u32 = 4 });
	shibe_store(vm, data, (shibe_cell_t){ .u32 = 0xaa });
	shibe_store(vm, (shibe_cell_t){ .u32 = data.u32 + 1 }, (shibe_cell_t){ .u32 = 0xbb });

	// Overlapping forward move, which needs memmove semantics
	LITU(data.u32 + 1);
	LITU(data.u32);
	LITU(2);
	EMIT(COPY);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", shibe_fetch(vm, data).u32, 0xaau);
	BTEST_EXPECT_EQUAL("%u", shibe_fetch(vm, (shibe_cell_t){ .u32 = data.u32 + 1 }).u32, 0xaau);
	BTEST_EXPECT_EQUAL("%u", shibe_fetch(vm, (shibe_cell_t){ .u32 = data.u32 + 2 }).u32, 0xbbu);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, division_is_total) {
	// A zero divisor yields a quotient of zero rather than faulting
	LIT(7); LIT(0); EMIT(SDIV);
	LIT(7); LIT(0); EMIT(UDIV);
	// and a remainder of the dividend, which is what keeps
	// lhs == (lhs / rhs) * rhs + lhs % rhs true for every input
	LIT(7); LIT(0); EMIT(SREM);
	LIT(7); LIT(0); EMIT(UREM);
	// The one signed division that overflows wraps instead
	LIT(INT32_MIN); LIT(-1); EMIT(SDIV);
	LIT(INT32_MIN); LIT(-1); EMIT(SREM);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 0);         // INT32_MIN % -1
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, INT32_MIN); // INT32_MIN / -1
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 7);         // UREM
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 7);         // SREM
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 0);         // UDIV
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 0);         // SDIV
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, shift_amount_is_masked) {
	// 33 masks to 1 rather than being left undefined
	LITU(1); LITU(33); EMIT(SHL);
	// A negative value shifts arithmetically
	LIT(-8); LITU(1); EMIT(SAR);
	// The logical counterpart does not sign extend
	LIT(-8); LITU(1); EMIT(SHR);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 0x7ffffffcu);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, -4);
	BTEST_EXPECT_EQUAL("%u", shibe_pop(vm).u32, 2u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, data_stack_underflow_faults) {
	EMIT(ADD);
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_STACK_UNDERFLOW);
	// arg 0 is the data stack
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, 0u);
}

BTEST(sexec, aux_stack_underflow_names_the_other_stack) {
	EMIT(RET);
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_STACK_UNDERFLOW);
	// arg 1 is the auxiliary stack
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, 1u);
}

BTEST(sexec, running_data_traps) {
	// Nothing was assembled here, so the cell is zero, which decodes as TRAP
	shibe_cell_t data = shibe_alloc(vm, SHIBE_MEM_REGION_3, (shibe_cell_t){ .u32 = 1 });
	BTEST_ASSERT(shibe_asm_end(sasm));

	BTEST_EXPECT_EQUAL("%d", shibe_execute(vm, data), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_TRAP);
}

BTEST(sexec, running_off_the_end_faults) {
	// One cell of code in a region of its own, so the refill after it faults
	shibe_cell_t tiny = shibe_alloc(vm, SHIBE_MEM_REGION_4, (shibe_cell_t){ .u32 = 1 });
	shibe_asm_t* small = shibe_asm_begin(vm, exec_allocator, tiny);
	shibe_asm_emit(small, SHIBE_OP_NOP);
	BTEST_ASSERT(shibe_asm_end(small));
	BTEST_ASSERT(shibe_asm_end(sasm));

	// NOP, then TRAP padding, so it faults before ever leaving the cell
	BTEST_EXPECT_EQUAL("%d", shibe_execute(vm, tiny), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_TRAP);
}

BTEST(sexec, extcall_reaches_the_host) {
	test_host.extcall = record_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	LIT(1);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	BTEST_EXPECT_EQUAL("%u", last_extcall.u32, 3u);
	// The host pushed 99 through the public api and execution carried on
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 100);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, debug_hook_reports_bundle_and_slot) {
	test_host.debug = record_step;

	// One full bundle with an operand cell, then a second bundle
	LIT(1); EMIT(DUP); EMIT(ADD); EMIT(DUP);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);

	BTEST_ASSERT_EQUAL("%d", num_steps, 5);
	BTEST_EXPECT_EQUAL("%u", steps[0].bundle.u32, code.u32);
	BTEST_EXPECT_EQUAL("%u", (uint32_t)steps[0].slot, 0u);
	BTEST_EXPECT_EQUAL("%u", (uint32_t)steps[1].slot, 1u);
	BTEST_EXPECT_EQUAL("%u", (uint32_t)steps[2].slot, 2u);
	BTEST_EXPECT_EQUAL("%u", (uint32_t)steps[3].slot, 3u);
	// The operand cell sits between the two bundles
	BTEST_EXPECT_EQUAL("%u", steps[4].bundle.u32, code.u32 + 2u);
	BTEST_EXPECT_EQUAL("%u", (uint32_t)steps[4].slot, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, extcall_failure_panics) {
	test_host.extcall = failing_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	// The host failed on its own, so this is a fresh panic naming the call
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_EXTCALL);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, 3u);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(sexec, extcall_relaying_a_panic_keeps_the_original_reason) {
	test_host.extcall = relaying_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	// Fired once, by the pop that failed, and not re-raised as an extcall error
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_STACK_UNDERFLOW);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(sexec, extcall_can_suspend) {
	test_host.extcall = suspending_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	// Suspending is not a failure, so nothing panicked
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_SUSPENDED);
}

BTEST(sexec, extcall_resumes_after_the_call) {
	test_host.extcall = suspending_extcall;

	// EXTCALL closes its bundle, so the work below it is in the next one
	LIT(2);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	LIT(5);
	EMIT(ADD);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	// Everything the run had done is still there
	BTEST_ASSERT_EQUAL("%u", depth(), 1u);

	// What a handler that suspends to wait on something does when the answer
	// turns up: leave the result behind, then let the run carry on
	shibe_push(vm, (shibe_cell_t){ .i32 = 10 });

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	// The call is not made a second time
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 17);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
}

BTEST(sexec, debug_hook_can_suspend_mid_bundle) {
	test_host.debug = suspending_hook;

	// One bundle of four opcodes, so the suspension lands in the middle of a
	// window that the resume has to pick up where it was left
	suspend_at = (shibe_op_addr_t){ .bundle = shibe_asm_here(sasm), .slot = 2 };
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_steps, 3);
	// The hook reports an opcode before it runs, so the ADD has not happened
	BTEST_EXPECT_EQUAL("%u", depth(), 2u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	// The ADD ran without being reported again, and the HALT after it was
	BTEST_EXPECT_EQUAL("%d", num_steps, 4);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, a_hook_that_always_suspends_single_steps) {
	test_host.debug = single_stepping_hook;

	shibe_cell_t bundle = shibe_asm_here(sasm);
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);

	// Four opcodes, so four suspensions and one resume that reaches the HALT.
	// The bound is what fails the test if a resume ever stands still.
	int num_resumes = 0;
	shibe_status_t status;
	while ((status = shibe_resume(vm)) == SHIBE_SUSPENDED && num_resumes < MAX_STEPS) {
		++num_resumes;
	}
	BTEST_EXPECT_EQUAL("%d", status, SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_resumes, 3);

	// Every opcode was reported exactly once, in order
	BTEST_ASSERT_EQUAL("%d", num_steps, 4);
	for (uint8_t i = 0; i < 4; ++i) {
		BTEST_EXPECT_EQUAL("%u", steps[i].bundle.u32, bundle.u32);
		BTEST_EXPECT_EQUAL("%u", (unsigned)steps[i].slot, (unsigned)i);
	}

	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, the_hook_can_be_dropped_while_suspended) {
	test_host.debug = suspending_hook;

	suspend_at = (shibe_op_addr_t){ .bundle = shibe_asm_here(sasm), .slot = 2 };
	LIT(1);
	LIT(2);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_ASSERT_EQUAL("%d", num_steps, 3);

	// The two interpreter builds have to agree about where a suspended run is:
	// this one is picked up by the one without a hook
	test_host.debug = NULL;

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_steps, 3);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 3);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, resume_without_a_suspension_is_rejected) {
	LIT(1);
	EMIT(HALT);

	// Idle: there is no activation to come back to
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
	clear_panic();

	// And a run that halted is over for good
	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(sexec, resuming_a_panicked_vm_reports_nothing_new) {
	test_host.extcall = failing_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_ASSERT_EQUAL("%d", num_panics, 1);

	// Same as shibe_execute: the panic already fired, so this only relays it
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
}

BTEST(sexec, debug_hook_failure_panics) {
	test_host.debug = failing_hook;

	LIT(1);
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	// It failed on the very first opcode
	BTEST_EXPECT_EQUAL("%d", num_steps, 1);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_HOOK);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(sexec, host_can_re_enter_the_vm) {
	test_host.extcall = reentrant_extcall;

	shibe_asm_label_t sub = shibe_asm_make_label(sasm);

	// The outer run, which calls out and then keeps going
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	// `LIT target; CALL; HALT`: the CALL and the RET below balance on the
	// auxiliary stack, and the HALT hands control back to the host rather than
	// popping a return address belonging to the outer run
	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	LIT(7);
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 1);
	// The nested run left 7 on the shared stack and the outer resumed where it
	// had left off, so it added its own 5
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	// Balanced, so the outer run's auxiliary stack came back untouched
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
}

BTEST(sexec, re_entry_while_suspended_is_rejected) {
	test_host.extcall = suspending_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_ASSERT_EQUAL("%d", num_panics, 0);

	// Only IDLE and RUNNING are re-entrant; a suspended run has to be resumed
	BTEST_EXPECT_EQUAL("%d", shibe_execute(vm, code), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(sexec, nested_run_can_suspend_with_a_continuation) {
	test_host.extcall = continuing_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	// Call 1 named a continuation, so the suspension may cross its boundary
	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 2);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_SUSPENDED);

	// The nested run finishes first, then call 1 is finished by call 3, and
	// only then does the run underneath carry on into its LIT 5; ADD
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);

	// The second half read back what the first half left in the frame
	BTEST_EXPECT_EQUAL("%d", observed_slot.i32, 42);

	// Everything was handed back on the way out
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
}

BTEST(sexec, a_continuation_can_suspend_again) {
	test_host.extcall = retrying_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);

	// A second half that is not ready is asked again rather than dragging the
	// run under it along, so the frame has to still be there each time
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 2);
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 3);

	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(sexec, a_leaf_call_can_suspend_and_be_finished_later) {
	test_host.extcall = deferring_extcall;

	// No re-entry anywhere: the call simply stops, and the rest of it runs
	// after the resume
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", depth(), 0u);

	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	// The second half left what the interrupted EXTCALL had promised, so the
	// run underneath cannot tell that anything happened in between
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(sexec, suspending_on_a_frame_with_no_continuation_is_rejected) {
	test_host.extcall = stranding_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	// Nothing could ever take the frame back, so the suspension is refused even
	// though the run underneath is a top level one that could have carried on
	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(sexec, a_leaf_call_can_open_a_frame) {
	test_host.extcall = annotating_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// A creator of 0 is what marks the frame as a host call's, whether or not
	// it ever came back into the vm
	BTEST_EXPECT_EQUAL("%u", observed_frame_head.u32, 0u);
	BTEST_EXPECT_EQUAL("%u", observed_slot.u32, 123u);
	// The vm called the host, so it takes the frame back when the call returns
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->fp.u32, 0u);
}

BTEST(sexec, a_call_from_outside_the_vm_can_be_resumable) {
	test_host.extcall = top_level_continuation_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);
	BTEST_ASSERT(shibe_asm_end(sasm));

	// Standing in for a host function the host called directly. It cannot tell
	// that from being called by the vm, and it does not have to: it takes a
	// frame and names its second half either way.
	shibe_frame_t frame = shibe_alloc_frame(vm, 1);
	shibe_set_local(vm, frame, 0, (shibe_cell_t){ .i32 = 77 });
	shibe_set_continuation(vm, frame, (shibe_cell_t){ .u32 = 5 });

	BTEST_ASSERT_EQUAL("%d", shibe_execute(vm, code), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);

	// The run it started finishes first, then its own second half runs, and
	// there is nothing underneath that one to carry on
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_continuations, 1);
	BTEST_EXPECT_EQUAL("%d", observed_slot.i32, 77);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_IDLE);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(sexec, a_frame_taken_outside_a_callback_is_the_hosts_to_free) {
	// Nothing called the host here, so nothing is going to take it back either
	shibe_frame_t frame = shibe_alloc_frame(vm, 2);
	BTEST_ASSERT(frame.fp != 0);
	BTEST_EXPECT(shibe_inspect(vm)->asp.u32 > 0u);

	shibe_set_local(vm, frame, 1, (shibe_cell_t){ .i32 = 5 });
	BTEST_EXPECT_EQUAL("%d", shibe_get_local(vm, frame, 1).i32, 5);
	// Untouched locals read as zero rather than as whatever the stack held
	BTEST_EXPECT_EQUAL("%d", shibe_get_local(vm, frame, 0).i32, 0);

	shibe_free_frame(vm, frame);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);

	// And the handle is stale now
	shibe_get_local(vm, frame, 0);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(sexec, a_local_outside_the_frame_panics) {
	test_host.extcall = bad_local_extcall;
	frame_locals = 1;
	bad_local = 1;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(sexec, asking_for_a_frame_the_call_never_took_panics) {
	// Being reached through a frame is what makes a continuation one, so a call
	// that never took one has no business asking for it
	test_host.extcall = frameless_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(sexec, asking_twice_grows_the_one_frame) {
	test_host.extcall = regrowing_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// The same frame came back, not a second one stacked on top of it
	BTEST_EXPECT_EQUAL("%u", observed_frame_head.u32, 1u);
	// and what was already in it survived the growth
	BTEST_EXPECT_EQUAL("%u", observed_slot.u32, 11u);
	// The vm still took the whole thing back on the way out
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(sexec, nested_extcall_suspension_is_rejected) {
	test_host.extcall = nested_suspending_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	LIT(5);
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	// A leaf extcall, but the run it belongs to was re-entered, and there is no
	// way back into the host call frame underneath it
	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 2);
	// Raised once, at the boundary the suspension could not cross, and only
	// relayed by the level above it
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
	// Ends the nest like any other panic, host frame left to walk
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 8u);

	// And there is nothing to come back to
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_ERROR);
}

BTEST(sexec, nested_hook_suspension_is_rejected) {
	test_host.extcall = reentrant_extcall;
	test_host.debug = suspending_hook;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	// The hook is the other way to suspend, and it is refused in a nested run
	// for the same reason, wherever inside a bundle it stops
	shibe_asm_bind_label(sasm, sub);
	suspend_at = (shibe_op_addr_t){ .bundle = shibe_asm_here(sasm), .slot = 1 };
	LIT(7);
	EMIT(DUP);
	EMIT(ADD);
	EMIT(RET);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
	BTEST_EXPECT(shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC);
}

BTEST(sexec, host_call_can_suspend_after_its_nested_run_finished) {
	test_host.extcall = reenter_then_suspend_extcall;

	// What matters is which host call suspends, not whether it had re-entered
	// the vm before it did: the nested run is over by then, and the frame it
	// opened is what the continuation is called through
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(ADD);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	shibe_asm_label_t sub = shibe_asm_make_label(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	shibe_asm_bind_label(sasm, sub);
	LIT(7);
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_SUSPENDED);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// The nested run gave its activation back, but the frame is the host call's
	// and that call has not finished
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);

	// The continuation pushes the 5 the first half never got to, and only then
	// does the interrupted run carry on into its ADD
	BTEST_EXPECT_EQUAL("%d", shibe_resume(vm), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 2);
	BTEST_EXPECT_EQUAL("%u", depth(), 1u);
	BTEST_EXPECT_EQUAL("%d", shibe_pop(vm).i32, 12);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);
	// And the frame went with it
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
}

BTEST(sexec, re_entry_leaves_a_walkable_boundary) {
	test_host.extcall = boundary_extcall;

	shibe_asm_label_t sub = shibe_asm_make_label(sasm);

	// The outer run resumes at the bundle after this one, which is what the
	// boundary has to record
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	shibe_cell_t outer_resume = shibe_asm_here(sasm);
	EMIT(HALT);

	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	EMIT_LABEL(LIT, sub);
	EMIT(CALL);
	EMIT(HALT);

	// The nested run calls out again, and that callback reads the boundary
	shibe_asm_bind_label(sasm, sub);
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 2 }));
	EMIT(RET);

	BTEST_ASSERT_EQUAL("%d", run(), SHIBE_OK);
	BTEST_EXPECT_EQUAL("%d", num_panics, 0);

	// A creator of 0 is what marks the frame as the host's
	BTEST_EXPECT_EQUAL("%u", observed_creator.u32, 0u);
	// and the cell under the header carries the outer run on
	BTEST_EXPECT_EQUAL("%u", observed_outer_ip.u32, outer_resume.u32);
	// saved_fp chains back to the outer run, which never entered a frame
	BTEST_EXPECT_EQUAL("%u", observed_saved_fp.u32, 0u);
	// and the boundary was popped again on the way out
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->asp.u32, 0u);
	BTEST_EXPECT_EQUAL("%u", shibe_inspect(vm)->fp.u32, 0u);
}

BTEST(sexec, unbalanced_re_entry_stub_is_caught) {
	test_host.extcall = reentrant_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	// A stub that opens a frame and never closes it, which would otherwise
	// leave the outer run's auxiliary stack somewhere it never put it
	shibe_asm_align(sasm);
	nested_entry = shibe_asm_here(sasm);
	EMIT_IMM(ENTER, ((shibe_cell_t){ .u32 = 0 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_INVALID);
}

BTEST(sexec, re_entry_exhausting_the_aux_stack_panics_once) {
	test_host.extcall = recursing_extcall;

	// Re-enters itself, so every level costs a boundary frame and none of them
	// ever unwind
	recursive_entry = code;
	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 1 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT(num_extcalls > 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_STACK_OVERFLOW);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, 1u);
	// Relayed back up through every level without being raised again
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
}

BTEST(sexec, extcall_zero_is_unbound) {
	// Installed and working, but call 0 never reaches it
	test_host.extcall = record_extcall;

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 0 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_extcalls, 0);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_UNBOUND);
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, 0u);
}

BTEST(sexec, extcall_without_a_handler_is_unbound) {
	// test_host.extcall is left NULL by the fixture

	EMIT_IMM(EXTCALL, ((shibe_cell_t){ .u32 = 3 }));
	EMIT(HALT);

	BTEST_EXPECT_EQUAL("%d", run(), SHIBE_ERROR);
	BTEST_EXPECT_EQUAL("%d", num_panics, 1);
	BTEST_EXPECT(last_panic.error == SHIBE_ERR_UNBOUND);
	// Same reason, and it still names the call the program asked for
	BTEST_EXPECT_EQUAL("%u", last_panic.arg.u32, 3u);
}
