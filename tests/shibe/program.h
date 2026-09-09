// The fixture shared by the suites that assemble a program and run it: a vm,
// an assembler pointed at a fresh block of code, and the counters the host
// callbacks in those suites record themselves in.
#include <btest.h>
#include <shibe.h>
#include <shibe/asm.h>
#include <shibe/opcode.h>
#include "common.h"

// The assembler gets an allocator of its own: shibe_asm_end restores it
static shibe_barena_t program_arena;
static shibe_allocator_t* program_allocator;

#define CODE_LEN 64u

static shibe_cell_t code;
static shibe_asm_t* sasm;

// Recorded by the hooks in either suite
#define MAX_STEPS 32
static shibe_op_addr_t steps[MAX_STEPS];
static int num_steps;
static int num_extcalls;

// Read back out of a host frame by the callbacks in either suite
static shibe_cell_t observed_slot;
static shibe_cell_t observed_frame_head;

// The stub a compiler would emit for an exported word, re-entered by the
// callback below
static shibe_cell_t nested_entry;

static inline shibe_status_t
reentrant_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)index;
	++num_extcalls;
	return shibe_execute(called, nested_entry);
}

// Fails on its own, without touching the vm
static inline shibe_status_t
failing_extcall(shibe_host_t* host, shibe_vm_t* called, shibe_cell_t index) {
	(void)host; (void)called; (void)index;
	++num_extcalls;
	return SHIBE_ERROR;
}

static inline void
init_per_program_test(void) {
	init_per_test();
	program_allocator = shibe_barena_init(&program_arena, &arena_pool);
	test_host.debug = NULL;
	test_host.extcall = NULL;
	num_steps = 0;
	num_extcalls = 0;
	observed_slot = (shibe_cell_t){ .u32 = 0xffffffffu };
	observed_frame_head = (shibe_cell_t){ .u32 = 0xffffffffu };
	nested_entry = (shibe_cell_t){ 0 };

	code = shibe_alloc(vm, SHIBE_MEM_REGION_1, (shibe_cell_t){ .u32 = CODE_LEN });
	sasm = shibe_asm_begin(vm, program_allocator, code);
}

static inline void
cleanup_per_program_test(void) {
	barena_reset(&program_arena.arena);
	program_arena.arena = (barena_t){ 0 };
	test_host.debug = NULL;
	test_host.extcall = NULL;
	cleanup_per_test();
}

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
