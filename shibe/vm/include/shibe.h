#ifndef SHIBE_H
#define SHIBE_H

#include <stdint.h>
#include <stddef.h>

#ifndef SHIBE_API
#define SHIBE_API
#endif

typedef struct shibe_vm_s shibe_vm_t;

typedef union {
	int32_t i32;
	uint32_t u32;
	float f32;
} shibe_cell_t;

typedef enum {
	SHIBE_OK,
	SHIBE_SUSPENDED,
	SHIBE_ERROR,
} shibe_status_t;

typedef enum {
	SHIBE_EXEC_IDLE,
	SHIBE_EXEC_RUNNING,
	SHIBE_EXEC_SUSPENDED,
	SHIBE_EXEC_PANIC,
} shibe_exec_state_t;

typedef enum {
	SHIBE_ERR_NONE,
	SHIBE_ERR_INVALID,
	// `arg` names the stack: 0 for the data stack, 1 for the auxiliary stack
	SHIBE_ERR_STACK_OVERFLOW,
	SHIBE_ERR_STACK_UNDERFLOW,
	SHIBE_ERR_OOM,
	// `arg` is the address that faulted
	SHIBE_ERR_MEM_FAULT,
	SHIBE_ERR_TRAP,
	// A host callback failed on its own. `arg` is the external call number, or
	// 0 when it was the debug hook. A callback that instead relays a panic
	// raised on this vm from underneath it keeps that panic's reason.
	SHIBE_ERR_HOST,
} shibe_error_t;

typedef struct {
	shibe_error_t error;
	shibe_cell_t arg;
} shibe_panic_t;

typedef struct {
	// registers
	shibe_cell_t ip;
	shibe_cell_t dsp;
	shibe_cell_t asp;
	shibe_cell_t fp;
	shibe_cell_t tp;
	shibe_cell_t tm;

	// stacks
	shibe_cell_t* ds;
	shibe_cell_t* as;

	shibe_exec_state_t exec_state;
} shibe_state_t;

// The fixed underlying type keeps the enumeration constants unsigned. Without
// it they are `int`, and shifting one into the region field of an address
// (`SHIBE_MEM_REGION_7 << 29`) overflows and is undefined
typedef enum : uint32_t {
	SHIBE_MEM_REGION_0,
	SHIBE_MEM_REGION_1,
	SHIBE_MEM_REGION_2,
	SHIBE_MEM_REGION_3,
	SHIBE_MEM_REGION_4,
	SHIBE_MEM_REGION_5,
	SHIBE_MEM_REGION_6,
	SHIBE_MEM_REGION_7,
} shibe_mem_region_t;

typedef struct shibe_allocator_s shibe_allocator_t;
struct shibe_allocator_s {
	void* (*alloc)(shibe_allocator_t* allocator, size_t size, size_t alignment);
	void* (*snapshot)(shibe_allocator_t* allocator);
	void (*restore)(shibe_allocator_t* allocator, void* snapshot);
};

// Where the interpreter is: the address of the bundle being executed and which
// of its four slots is running. Addresses are cell addresses throughout, so an
// opcode is named by a base and an offset rather than by an address of its own.
typedef struct {
	shibe_cell_t bundle;
	uint8_t slot;
} shibe_op_addr_t;

typedef struct shibe_host_s shibe_host_t;
struct shibe_host_s {
	void (*panic)(shibe_host_t* host, shibe_vm_t* vm, const shibe_panic_t* panic);
	shibe_status_t (*debug)(shibe_host_t* host, shibe_vm_t* vm, const shibe_state_t* state, shibe_op_addr_t at);
	shibe_status_t (*extcall)(shibe_host_t* host, shibe_vm_t* vm, shibe_cell_t index);
};

typedef struct {
	uint32_t ds_len;
	uint32_t as_len;

	shibe_allocator_t* allocator;
	shibe_host_t* host;
} shibe_config_t;

SHIBE_API shibe_vm_t*
shibe_create(shibe_config_t config);

SHIBE_API void
shibe_destroy(shibe_vm_t* vm);

SHIBE_API shibe_vm_t*
shibe_fork(shibe_vm_t* vm, const shibe_config_t* config);

SHIBE_API void
shibe_reset(shibe_vm_t* vm);

SHIBE_API shibe_cell_t
shibe_alloc(shibe_vm_t* vm, shibe_mem_region_t region, shibe_cell_t num_cells);

SHIBE_API shibe_cell_t
shibe_fetch(shibe_vm_t* vm, shibe_cell_t vm_addr);

SHIBE_API void
shibe_store(shibe_vm_t* vm, shibe_cell_t vm_addr, shibe_cell_t value);

SHIBE_API void
shibe_copy_to_vm  (shibe_vm_t* vm, shibe_cell_t vm_addr, const void* host_addr, uint32_t num_bytes);

SHIBE_API void
shibe_copy_to_host(shibe_vm_t* vm, shibe_cell_t vm_addr,       void* host_addr, uint32_t num_bytes);

SHIBE_API void
shibe_push(shibe_vm_t* vm, shibe_cell_t item);

SHIBE_API shibe_cell_t
shibe_pop(shibe_vm_t* vm);

/**
 * Run from `addr` until HALT, a fault, or a suspension
 *
 * Callable from inside a host callback to re-enter the same vm. The entry point
 * of such a call has to return to the host rather than run off the end of the
 * outer run's auxiliary stack, so it must be a stub that halts, of the shape
 * `LIT target; CALL; HALT`. The CALL and its RET balance out on the auxiliary
 * stack, leaving the outer run's frames untouched.
 *
 * A re-entry pushes a frame of its own so that a stack walker can cross the
 * host boundary. The frame consists of:
 *
 * - The outer run's `ip` where a CALL would have left its return
 * - An auxiliary frame header whose `creator` is 0
 *
 * See shibe/opcode.h for the layout and what a walker has to do about it.
 *
 * A stub that does not hand the auxiliary stack back as it found it panics with
 * SHIBE_ERR_INVALID rather than corrupting the outer run.
 *
 * A panic or a suspension inside a nested run ends the whole nest. The boundary
 * frame is left in place so the host can still walk the stack afterwards.
 */
SHIBE_API shibe_status_t
shibe_execute(shibe_vm_t* vm, shibe_cell_t addr);

SHIBE_API shibe_status_t
shibe_resume(shibe_vm_t* vm);

SHIBE_API const shibe_state_t*
shibe_inspect(shibe_vm_t* vm);

// An address packs the region into the top bits and a cell index into the rest,
// which is what limits a region to 2^SHIBE_MEM_INDEX_BITS cells
#define SHIBE_MEM_REGION_BITS 3
#define SHIBE_MEM_INDEX_BITS  29
#define SHIBE_MEM_REGION_MASK (((uint32_t)1 << SHIBE_MEM_REGION_BITS) - 1)
#define SHIBE_MEM_INDEX_MASK  (((uint32_t)1 << SHIBE_MEM_INDEX_BITS) - 1)

static inline shibe_mem_region_t
shibe_mem_region(shibe_cell_t addr) {
	return (shibe_mem_region_t)(addr.u32 >> SHIBE_MEM_INDEX_BITS);
}

static inline uint32_t
shibe_mem_index(shibe_cell_t addr) {
	return addr.u32 & SHIBE_MEM_INDEX_MASK;
}

// Build an address out of a region and a cell index.
// Both halves are masked, so an index running off the end of a region wraps
// inside it instead of silently landing in the next one.
static inline shibe_cell_t
shibe_mem_addr(shibe_mem_region_t region, uint32_t index) {
	return (shibe_cell_t){
		.u32 = (((uint32_t)region & SHIBE_MEM_REGION_MASK) << SHIBE_MEM_INDEX_BITS)
			| (index & SHIBE_MEM_INDEX_MASK)
	};
}

#endif
