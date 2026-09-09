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
	// The external call handler failed on its own. `arg` is the call number.
	SHIBE_ERR_EXTCALL,
	// The debug hook failed on its own. `arg` is unused.
	SHIBE_ERR_HOOK,
	// An EXTCALL reached no handler: either none is installed, or it was call
	// number 0, which is reserved so that an unpached operand cell cannot
	// dispatch anywhere. `arg` is the call number.
	SHIBE_ERR_UNBOUND,

	// None of the three is raised by a callback that merely relays a panic
	// raised on this vm from underneath it. That keeps its own reason.
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

/**
 * A frame belonging to a host call, living on the auxiliary stack.
 *
 * Handed out by @ref shibe_alloc_frame and @ref shibe_get_frame.
 */
typedef struct { uint32_t fp; } shibe_frame_t;

/*! The handle no frame ever has */
#define SHIBE_NO_FRAME ((shibe_frame_t){ 0 })

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
 * A re-entry implicitly pushes a frame if the caller has not allocated one yet.
 * See shibe/opcode.h for the layout and what a walker has to do about it.
 *
 * A stub that does not hand the auxiliary stack back as it found it panics with
 * `SHIBE_ERR_INVALID` rather than corrupting the outer run.
 *
 * A panic inside a nested run ends the whole chain. The frames are left in place
 * so the host can still walk the stack afterwards.
 *
 * To make this call suspendable, the caller has to call @ref shibe_set_continuation.
 * Otherwise, a call lower in the chain returning @ref SHIBE_SUSPENDED would panic
 * with @ref SHIBE_ERR_INVALID instead.
 */
SHIBE_API shibe_status_t
shibe_execute(shibe_vm_t* vm, shibe_cell_t addr);

/**
 * Give the running host function a frame with `num_locals` cells.
 *
 * A frame is where a host function can keep its data so that it can be inspected
 * by the VM.
 * For example, it can store `__FILE__` and `__LINE__` so a stack walker can
 * even print out C frame's location interleaved with VM frames.
 * In the case of suspension, this data would outlive the C stack frame.
 *
 * Allocating one before @ref shibe_execute, along with @ref
 * shibe_set_continuation is what makes a host function resumable.
 *
 * A host call gets one frame however many times this is asked, so a second call
 * grows it rather than stacking another.
 *
 * No freeing is needed when this is done within a callback. The VM will
 * automatically free the frame when the callback returns without suspension.
 *
 * Returns `SHIBE_NO_FRAME` if the frame could not be allocated.
 */
SHIBE_API shibe_frame_t
shibe_alloc_frame(shibe_vm_t* vm, uint32_t num_locals);

/**
 * Release a frame allocated outside of a callback.
 *
 * Only needed for a host function that allocated one outside of a callback
 * and did not suspend.
 */
SHIBE_API void
shibe_free_frame(shibe_vm_t* vm, shibe_frame_t frame);

/**
 * Retrieve a previously allocated frame.
 *
 * When a host function is suspended, it returns with @ref SHIBE_SUSPENDED and
 * the C frame is destroyed.
 * To resume its work, it needs to call @ref shibe_set_continuation.
 * The continuation will be called when the VM is resumed through @ref shibe_resume.
 *
 * The frame is how continuations pass data from one to another or set the next
 * continuation.
 */
SHIBE_API shibe_frame_t
shibe_get_frame(shibe_vm_t* vm);

/**
 * Name the extcall that finishes this host call after a suspension.
 *
 * When a host function lower in the call chain returns `SHIBE_SUSPENDED`, the
 * current execution is suspended. By the time @ref shibe_resume is called this
 * function has already returned and cannot be restarted, so the vm makes an
 * extcall to `continuation` instead, with `frame` in front of it.
 *
 * That call stands in for the rest of this function, and it has to leave the
 * data stack the way the interrupted `EXTCALL` promised: it is that call,
 * finished late.
 *
 * A function making several calls into the vm can name a different resume point
 * before each one.
 *
 * Slot 0 is reserved to mean "unbound", so leaving it unset, or setting it back
 * to 0, marks the next run as non-suspendable, and any attempt to suspend it
 * panics with `SHIBE_ERR_INVALID`.
 */
SHIBE_API void
shibe_set_continuation(shibe_vm_t* vm, shibe_frame_t frame, shibe_cell_t continuation);

/*! Write one of a frame's locals. Out of range panics with `SHIBE_ERR_INVALID` */
SHIBE_API void
shibe_set_local(shibe_vm_t* vm, shibe_frame_t frame, uint32_t index, shibe_cell_t value);

/*! Read one of a frame's locals. Out of range panics with `SHIBE_ERR_INVALID` */
SHIBE_API shibe_cell_t
shibe_get_local(shibe_vm_t* vm, shibe_frame_t frame, uint32_t index);

/**
 * Carry on a run that a host callback suspended
 *
 * A run suspends when the extcall handler or the debug hook returns
 * `SHIBE_SUSPENDED`. The host is free to work on the vm in between.
 * Then, it could push a result and resume execution, turning an asynchronous
 * call into a synchronous blocking call for the VM.
 *
 * Where the run picks up depends on which callback stopped it:
 *
 * - An extcall carries on after the call. It is not made a second time, so a
 *   handler that suspends runs its side of the call exactly once.
 * - The debug hook carries on at the instruction it was reporting, which had
 *   not run yet, and does not report it again. A hook that always suspends
 *   therefore single steps rather than standing still.
 *
 * Anything other than a suspended vm panics with `SHIBE_ERR_INVALID`: an idle
 * one has no execution to continue, a halted run is over for good, and a running
 * one is already inside the interpreter. A panicked vm returns `SHIBE_ERROR`
 * without panicking again, like @ref shibe_execute.
 */
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
