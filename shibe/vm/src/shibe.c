#include "internal.h"
#include <shibe/opcode.h>

static shibe_host_t shibe_dummy_host = { 0 };

#if defined(__clang__) || defined(__GNUC__)
__attribute__((cold, noinline))
#endif
static void
shibe_panic(shibe_vm_t* vm, const shibe_panic_t* panic) {
	shibe_host_t* host = vm->config.host;
	if (host->panic != NULL) {
		host->panic(host, vm, panic);
	}

	vm->state.exec_state = SHIBE_EXEC_PANIC;
}

static bool
shibe_panicked(shibe_vm_t* vm) {
	return vm->state.exec_state == SHIBE_EXEC_PANIC;
}

static inline void*
shibe_memalign(shibe_allocator_t* allocator, size_t size, size_t alignment) {
	return allocator->alloc(allocator, size, alignment);
}

static inline void*
shibe_snapshot(shibe_allocator_t* allocator) {
	return allocator->snapshot(allocator);
}

static inline void
shibe_restore(shibe_allocator_t* allocator, void* snapshot) {
	allocator->restore(allocator, snapshot);
}

static inline void*
shibe_realloc(void* ptr, size_t size, shibe_vm_t* vm) {
	if (size != 0) {
		void* mem = shibe_memalign(vm->config.allocator, size, _Alignof(shibe_cell_t));
		if (mem == NULL) {
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_OOM,
			});
		}
		return mem;
	} else {
		return NULL;
	}
}

shibe_vm_t*
shibe_create(shibe_config_t config) {
	shibe_allocator_t* allocator = config.allocator;
	if (allocator == NULL) { return NULL; }

	if (config.ds_len == 0) { config.ds_len = 256; }
	if (config.as_len == 0) { config.as_len = 1024; }
	if (config.host == NULL) { config.host = &shibe_dummy_host; }

	void* snapshot = shibe_snapshot(allocator);
	shibe_vm_t* vm = shibe_memalign(allocator, sizeof(shibe_vm_t), _Alignof(shibe_vm_t));
	if (vm == NULL) { return NULL; }
	*vm = (shibe_vm_t){
		.config = config,
		.snapshot = snapshot,
	};

	// Address 0 is special
	bseg_resize(vm->regions[0], 1, vm);
	if (shibe_panicked(vm)) {
		shibe_restore(allocator, snapshot);
		return NULL;
	}

	vm->state.ds = shibe_memalign(allocator, sizeof(shibe_cell_t) * config.ds_len, _Alignof(shibe_cell_t));
	if (vm->state.ds == NULL) {
		shibe_restore(allocator, snapshot);
		return NULL;
	}

	vm->state.as = shibe_memalign(allocator, sizeof(shibe_cell_t) * config.as_len, _Alignof(shibe_cell_t));
	if (vm->state.as == NULL) {
		shibe_restore(allocator, snapshot);
		return NULL;
	}

	shibe_reset(vm);

	return vm;
}

void
shibe_destroy(shibe_vm_t* vm) {
	shibe_restore(vm->config.allocator, vm->snapshot);
}

void
shibe_reset(shibe_vm_t* vm) {
	if (vm->state.exec_state == SHIBE_EXEC_RUNNING) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return;
	}

	vm->state.ip =
	vm->state.dsp =
	vm->state.asp =
	vm->state.fp =
	vm->state.tp =
	vm->state.tm = SHIBE_ZERO;

	vm->state.exec_state = SHIBE_EXEC_IDLE;
	// The activation they belonged to is gone, so there is nothing to come back
	// to and no host call left owning a frame
	vm->suspension = (shibe_suspension_t){ 0 };
	vm->hfp = 0;
}

shibe_cell_t
shibe_alloc(shibe_vm_t* vm, shibe_mem_region_t region, shibe_cell_t num_cells) {
	if (num_cells.i32 < 0) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return SHIBE_ZERO;
	}

	region = region & 0x07;
	shibe_mem_seg_t* mem_seg = &vm->regions[region];
	size_t len = bseg_len(*mem_seg);
	size_t new_len = len + (size_t)num_cells.u32;
	if (new_len <= SHIBE_REGION_MAX_LEN) {
		bseg_resize(*mem_seg, new_len, vm);
		return shibe_mem_addr(region, (uint32_t)len);
	} else {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_OOM,
		});
		return SHIBE_ZERO;
	}
}

shibe_cell_t
shibe_fetch(shibe_vm_t* vm, shibe_cell_t vm_addr) {
	shibe_mem_region_t region = shibe_mem_region(vm_addr);
	shibe_mem_seg_t* mem_seg = &vm->regions[region];
	uint32_t index = shibe_mem_index(vm_addr);

	if (index < bseg_len(*mem_seg)) {
		return bseg_at(*mem_seg, index);
	} else {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_MEM_FAULT,
			.arg = vm_addr,
		});
		return SHIBE_ZERO;
	}
}

void
shibe_store(shibe_vm_t* vm, shibe_cell_t vm_addr, shibe_cell_t value) {
	shibe_mem_region_t region = shibe_mem_region(vm_addr);
	shibe_mem_seg_t* mem_seg = &vm->regions[region];
	uint32_t index = shibe_mem_index(vm_addr);

	if (index < bseg_len(*mem_seg)) {
		bseg_at(*mem_seg, index) = value;
	} else {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_MEM_FAULT,
			.arg = vm_addr,
		});
	}
}

// Bytes sit in a cell in little-endian order on every host so that VM memory is
// portable and agrees with SHIBE_OP_BFETCH / SHIBE_OP_BSTORE.
// Going through bytes also means the host buffer needs no alignment.
static inline shibe_cell_t
shibe_cell_patch_le(shibe_cell_t value, const char* src, uint32_t num_bytes) {
	const unsigned char* bytes = (const unsigned char*)src;
	for (uint32_t i = 0; i < num_bytes; ++i) {
		uint32_t shift = i * 8;
		value.u32 =
			(value.u32 & ~((uint32_t)0xff << shift))
			| ((uint32_t)bytes[i] << shift);
	}
	return value;
}

static inline void
shibe_cell_extract_le(char* dst, shibe_cell_t value, uint32_t num_bytes) {
	unsigned char* bytes = (unsigned char*)dst;
	for (uint32_t i = 0; i < num_bytes; ++i) {
		bytes[i] = (unsigned char)(value.u32 >> (i * 8));
	}
}

void
shibe_copy_to_vm  (shibe_vm_t* vm, shibe_cell_t vm_addr, const void* host_addr, uint32_t num_bytes) {
	const char* src = host_addr;
	uint32_t num_cells = num_bytes / sizeof(shibe_cell_t);
	uint32_t num_tail_bytes = num_bytes % sizeof(shibe_cell_t);

	uint32_t i = 0;
	for (; i < num_cells && !shibe_panicked(vm); ++i) {
		shibe_cell_t value = shibe_cell_patch_le(
			SHIBE_ZERO, src + i * sizeof(shibe_cell_t), sizeof(shibe_cell_t)
		);
		shibe_store(vm, (shibe_cell_t){ vm_addr.u32 + i}, value);
	}

	// Read-modify-write the partial cell so only num_bytes bytes are touched
	if (num_tail_bytes > 0 && !shibe_panicked(vm)) {
		shibe_cell_t value = shibe_fetch(vm, (shibe_cell_t){ vm_addr.u32 + i});
		if (shibe_panicked(vm)) { return; }
		value = shibe_cell_patch_le(
			value, src + i * sizeof(shibe_cell_t), num_tail_bytes
		);
		shibe_store(vm, (shibe_cell_t){ vm_addr.u32 + i}, value);
	}
}

void
shibe_copy_to_host(shibe_vm_t* vm, shibe_cell_t vm_addr,       void* host_addr, uint32_t num_bytes) {
	char* dst = host_addr;
	uint32_t num_cells = num_bytes / sizeof(shibe_cell_t);
	uint32_t num_tail_bytes = num_bytes % sizeof(shibe_cell_t);

	uint32_t i = 0;
	for (; i < num_cells && !shibe_panicked(vm); ++i) {
		shibe_cell_t value = shibe_fetch(vm, (shibe_cell_t){ vm_addr.u32 + i});
		if (shibe_panicked(vm)) { return; }
		shibe_cell_extract_le(
			dst + i * sizeof(shibe_cell_t), value, sizeof(shibe_cell_t)
		);
	}

	if (num_tail_bytes > 0 && !shibe_panicked(vm)) {
		shibe_cell_t value = shibe_fetch(vm, (shibe_cell_t){ vm_addr.u32 + i});
		if (shibe_panicked(vm)) { return; }
		shibe_cell_extract_le(
			dst + i * sizeof(shibe_cell_t), value, num_tail_bytes
		);
	}
}

void
shibe_push(shibe_vm_t* vm, shibe_cell_t item) {
	if (vm->state.dsp.u32 < vm->config.ds_len) {
		vm->state.ds[vm->state.dsp.u32++] = item;
	} else {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_STACK_OVERFLOW,
		});
	}
}

shibe_cell_t
shibe_pop(shibe_vm_t* vm) {
	if (vm->state.dsp.u32 > 0) {
		return vm->state.ds[--vm->state.dsp.u32];
	} else {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_STACK_UNDERFLOW,
		});
		return SHIBE_ZERO;
	}
}

const shibe_state_t*
shibe_inspect(shibe_vm_t* vm) {
	return &vm->state;
}

static shibe_status_t
shibe_execute_with_hook(shibe_vm_t* vm, bool resuming);

static shibe_status_t
shibe_execute_without_hook(shibe_vm_t* vm, bool resuming);

// Creating 2 separate versions is the only way to have optimized opcode
// dispatch when no debug hook is attached. Which one runs is decided per run,
// so a hook may be attached or dropped while a run is suspended: what carries
// across is the suspension record, which both variants read and write the same
// way.
static inline shibe_status_t
shibe_run(shibe_vm_t* vm, bool resuming) {
	if (vm->config.host->debug == NULL) {
		return shibe_execute_without_hook(vm, resuming);
	} else {
		return shibe_execute_with_hook(vm, resuming);
	}
}

// Lays a frame for a host call on top of the auxiliary stack. Everything below
// `fp` describes the call; everything above is the host's to write.
static bool
shibe_open_host_frame(shibe_vm_t* vm, uint32_t num_locals) {
	uint32_t base = vm->state.asp.u32;
	uint64_t frame_len = (uint64_t)SHIBE_AUX_HOST_HEADER_LEN + num_locals;
	if ((uint64_t)(vm->config.as_len - base) < frame_len) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_STACK_OVERFLOW,
			.arg = SHIBE_STACK_AS,
		});
		return false;
	}

	// A run that is going is the one this call interrupted, and its `ip` is
	// where it carries on. A call made from outside the vm has nothing
	// underneath it, and address 0 is reserved, so it cannot be mistaken for a
	// resume point.
	shibe_cell_t outer_ip = vm->state.exec_state == SHIBE_EXEC_RUNNING
		? vm->state.ip
		: SHIBE_ZERO;

	shibe_cell_t* as = vm->state.as;
	as[base + 0] = outer_ip;
	as[base + 1] = SHIBE_ZERO;
	as[base + 2] = (shibe_cell_t){ .u32 = num_locals };
	as[base + 3] = vm->state.dsp;
	as[base + 4] = vm->state.fp;
	as[base + 5] = vm->state.tm;
	// Nothing else can produce this: address 0 is reserved, so no ENTER
	// operand cell ever lives there
	as[base + 6] = SHIBE_ZERO;

	uint32_t fp = base + SHIBE_AUX_HOST_HEADER_LEN;
	// Locals read as zero rather than as whatever the auxiliary stack last
	// held, so a call that fills only some of them still knows what it is
	// looking at
	for (uint32_t i = 0; i < num_locals; ++i) { as[fp + i] = SHIBE_ZERO; }

	vm->state.fp.u32 = fp;
	vm->state.asp.u32 = fp + num_locals;
	vm->hfp = fp;
	return true;
}

// Hands the auxiliary stack back to whatever was underneath the frame at `fp`.
// `dsp` is not restored: the data stack is how a call carries results out, the
// same way it is for a nested run that returns normally.
static void
shibe_close_host_frame(shibe_vm_t* vm, uint32_t fp) {
	uint32_t base = fp - SHIBE_AUX_HOST_HEADER_LEN;
	vm->state.fp = vm->state.as[base + 4];
	vm->state.asp.u32 = base;
	vm->hfp = 0;
}

// Whether the frame directly below `fp` is a host frame. `creator` is 0 for one
// and the address of an ENTER operand for anything a word built, and address 0
// is reserved, so the two can never be confused.
static bool
shibe_is_host_frame(shibe_vm_t* vm, uint32_t fp) {
	return fp >= SHIBE_AUX_HOST_HEADER_LEN
		&& vm->state.as[fp - 1].u32 == 0;
}

// A handle only names the frame of the call that is running. Anything else is a
// host holding on to one that has been taken back, or reaching for a frame that
// was never its own.
static bool
shibe_check_frame(shibe_vm_t* vm, shibe_frame_t frame) {
	if (shibe_panicked(vm)) { return false; }

	if (frame.fp == 0 || frame.fp != vm->hfp) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return false;
	}

	return true;
}

// Resolves a local against a frame. Out of range panics rather than reading as
// zero the way AGET does: the opcode tolerates a bad index because that is a
// miscompile the vm should not fault on, while a host indexing outside the
// frame it asked for is a plain bug.
static shibe_cell_t*
shibe_local_ref(shibe_vm_t* vm, shibe_frame_t frame, uint32_t index) {
	if (!shibe_check_frame(vm, frame)) { return NULL; }

	if (index >= vm->state.as[frame.fp + SHIBE_AUX_HOST_NUM_LOCALS].u32) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return NULL;
	}

	return &vm->state.as[frame.fp + index];
}

shibe_frame_t
shibe_alloc_frame(shibe_vm_t* vm, uint32_t num_locals) {
	if (shibe_panicked(vm)) { return SHIBE_NO_FRAME; }

	// A frame belongs to a host call, and a suspended vm is between calls
	shibe_exec_state_t exec_state = vm->state.exec_state;
	if (exec_state != SHIBE_EXEC_IDLE && exec_state != SHIBE_EXEC_RUNNING) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return SHIBE_NO_FRAME;
	}

	if (vm->hfp == 0) {
		if (!shibe_open_host_frame(vm, num_locals)) { return SHIBE_NO_FRAME; }
		return (shibe_frame_t){ .fp = vm->hfp };
	}

	// The call already has one, so this grows it rather than stacking another.
	// It is the topmost thing on the auxiliary stack whenever the call itself is
	// running, which is the only time this can be reached.
	uint32_t fp = vm->hfp;
	shibe_cell_t* have = &vm->state.as[fp + SHIBE_AUX_HOST_NUM_LOCALS];
	if (num_locals > have->u32) {
		if (vm->state.asp.u32 != fp + have->u32) {
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_INVALID,
			});
			return SHIBE_NO_FRAME;
		}
		if (vm->config.as_len - fp < num_locals) {
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_STACK_OVERFLOW,
				.arg = SHIBE_STACK_AS,
			});
			return SHIBE_NO_FRAME;
		}

		for (uint32_t i = have->u32; i < num_locals; ++i) {
			vm->state.as[fp + i] = SHIBE_ZERO;
		}
		have->u32 = num_locals;
		vm->state.asp.u32 = fp + num_locals;
	}

	return (shibe_frame_t){ .fp = fp };
}

void
shibe_free_frame(shibe_vm_t* vm, shibe_frame_t frame) {
	if (!shibe_check_frame(vm, frame)) { return; }
	shibe_close_host_frame(vm, frame.fp);
}

shibe_frame_t
shibe_get_frame(shibe_vm_t* vm) {
	if (shibe_panicked(vm)) { return SHIBE_NO_FRAME; }

	if (vm->hfp == 0) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return SHIBE_NO_FRAME;
	}

	return (shibe_frame_t){ .fp = vm->hfp };
}

void
shibe_set_continuation(shibe_vm_t* vm, shibe_frame_t frame, shibe_cell_t continuation) {
	if (!shibe_check_frame(vm, frame)) { return; }
	vm->state.as[frame.fp + SHIBE_AUX_HOST_CONTINUATION] = continuation;
}

void
shibe_set_local(shibe_vm_t* vm, shibe_frame_t frame, uint32_t index, shibe_cell_t value) {
	shibe_cell_t* ref = shibe_local_ref(vm, frame, index);
	if (ref != NULL) { *ref = value; }
}

shibe_cell_t
shibe_get_local(shibe_vm_t* vm, shibe_frame_t frame, uint32_t index) {
	shibe_cell_t* ref = shibe_local_ref(vm, frame, index);
	return ref != NULL ? *ref : SHIBE_ZERO;
}

shibe_status_t
shibe_execute(shibe_vm_t* vm, shibe_cell_t addr) {
	if (shibe_panicked(vm)) {
		return SHIBE_ERROR;
	}

	// A host callback is allowed to call back in on the same vm, so a run may
	// start on top of one that is already going
	shibe_exec_state_t caller_state = vm->state.exec_state;
	if (caller_state != SHIBE_EXEC_IDLE && caller_state != SHIBE_EXEC_RUNNING) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return SHIBE_ERROR;
	}

	// Both stacks and all of the other registers stay shared, which is how
	// arguments and results cross the boundary. `ip` and `fp` belong to a
	// single activation; the callback site puts the caller's back.
	//
	// A re-entry gets a frame whether or not the call asked for one, so that a
	// walker can cross the boundary and the outer run's resume point is written
	// down somewhere.
	bool nested = caller_state == SHIBE_EXEC_RUNNING;
	if (nested && vm->hfp == 0) {
		if (!shibe_open_host_frame(vm, 0)) { return SHIBE_ERROR; }
	}

	uint32_t frame = vm->hfp;
	uint32_t frame_asp = vm->state.asp.u32;
	shibe_cell_t frame_fp = vm->state.fp;

	vm->state.ip = addr;
	vm->state.exec_state = SHIBE_EXEC_RUNNING;

	shibe_status_t status = shibe_run(vm, false);

	if (status == SHIBE_SUSPENDED) {
		// A frame that outlives the suspension needs somebody to finish it:
		// this call frame is gone by the time a resume could happen, so without
		// a continuation neither the frame nor whatever ran underneath it could
		// ever be picked up again. A call that has no frame has nothing to come
		// back to and needs none.
		if (frame != 0
		 && vm->state.as[frame + SHIBE_AUX_HOST_CONTINUATION].u32 == 0) {
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_INVALID,
			});
			status = SHIBE_ERROR;
		}
	} else if (status == SHIBE_OK && nested) {
		// The entry point has to be a stub that halts, so it must give the
		// auxiliary stack back exactly as it found it
		if (vm->state.asp.u32 != frame_asp || vm->state.fp.u32 != frame_fp.u32) {
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_INVALID,
			});
			status = SHIBE_ERROR;
		} else {
			// The frame outlives this run: it belongs to the callback, which is
			// still going, and the callback site takes it back on the way out.
			// Only `ip` goes back now, so that a callback which suspends after
			// this records where the run underneath carries on rather than
			// where the run that just finished stopped.
			vm->state.ip = vm->state.as[frame + SHIBE_AUX_HOST_OUTER_IP];
			vm->state.exec_state = SHIBE_EXEC_RUNNING;
		}
	}

	return status;
}

// Finishes one orphaned host frame: its call frame returned on the way out of
// the suspension, so the continuation runs in place of the rest of it, with the
// frame current so it reads back whatever the first half stored.
static shibe_status_t
shibe_call_continuation(shibe_vm_t* vm, uint32_t fp, shibe_cell_t continuation) {
	shibe_host_t* host = vm->config.host;
	if (host->extcall == NULL) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_UNBOUND,
			.arg = continuation,
		});
		return SHIBE_ERROR;
	}

	// Being called consumes it. A second half that means to stop again, or to
	// start a run that might, has to name the next one out loud rather than
	// inherit the one that brought it here: that value was chosen for a
	// suspension that has already been dealt with, so acting on it again is a
	// bug every time.
	vm->state.as[fp + SHIBE_AUX_HOST_CONTINUATION] = SHIBE_ZERO;

	vm->hfp = fp;
	vm->state.exec_state = SHIBE_EXEC_RUNNING;
	shibe_status_t status = host->extcall(host, vm, continuation);
	vm->hfp = 0;

	// A callback that panicked the vm from underneath keeps its own reason,
	// exactly as one reached from the interpreter does
	if (shibe_panicked(vm)) { return SHIBE_ERROR; }

	if (status == SHIBE_ERROR) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_EXTCALL,
			.arg = continuation,
		});
		return SHIBE_ERROR;
	}

	return status;
}

shibe_status_t
shibe_resume(shibe_vm_t* vm) {
	if (shibe_panicked(vm)) {
		return SHIBE_ERROR;
	}

	// Only a suspended run has somewhere to come back to. An idle vm has no
	// activation at all, and a running one is already inside the interpreter.
	if (vm->state.exec_state != SHIBE_EXEC_SUSPENDED) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
		});
		return SHIBE_ERROR;
	}

	// Whatever the host did to the vm in the meantime stands: the registers and
	// both stacks are read back as they are now, which is how an extcall that
	// suspended leaves its result behind.
	shibe_status_t status;
	if (vm->suspension.at_continuation) {
		// A continuation was the one that suspended, so there is no run to pick
		// up: go straight back to calling it
		vm->suspension.at_continuation = false;
		status = SHIBE_OK;
	} else {
		vm->state.exec_state = SHIBE_EXEC_RUNNING;
		status = shibe_run(vm, true);
	}

	// Every host frame still standing was orphaned by the suspension. Nothing
	// is running underneath them, so each one is finished here, innermost
	// first, and the run below it carried on.
	while (status == SHIBE_OK) {
		uint32_t fp = vm->state.fp.u32;
		if (!shibe_is_host_frame(vm, fp)) { break; }

		shibe_cell_t continuation = vm->state.as[fp + SHIBE_AUX_HOST_CONTINUATION];
		if (continuation.u32 == 0) {
			// Every frame the unwind reaches was orphaned by the suspension,
			// and one that named nothing could not have suspended in the first
			// place, so this is a frame nothing can take back
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_INVALID,
			});
			return SHIBE_ERROR;
		}

		shibe_cell_t outer_ip = vm->state.as[fp + SHIBE_AUX_HOST_OUTER_IP];

		status = shibe_call_continuation(vm, fp, continuation);
		if (status == SHIBE_ERROR) { return SHIBE_ERROR; }
		if (status == SHIBE_SUSPENDED) {
			if (vm->state.exec_state != SHIBE_EXEC_SUSPENDED) {
				// It stopped on its own, not relaying the result so it must have
				// a frame with continuation
				if (vm->state.as[fp + SHIBE_AUX_HOST_CONTINUATION].u32 == 0) {
					shibe_panic(vm, &(shibe_panic_t){
						.error = SHIBE_ERR_INVALID,
					});
					return SHIBE_ERROR;
				}
				vm->suspension.at_continuation = true;
			}
			vm->state.exec_state = SHIBE_EXEC_SUSPENDED;
			return SHIBE_SUSPENDED;
		}

		shibe_close_host_frame(vm, fp);

		if (outer_ip.u32 == 0) {
			// A frame a top level call opened: there is no run underneath it,
			// so finishing it finishes everything
			vm->state.exec_state = SHIBE_EXEC_IDLE;
			break;
		}

		vm->state.ip = outer_ip;
		vm->state.exec_state = SHIBE_EXEC_RUNNING;
		status = shibe_run(vm, false);
	}

	return status;
}

// The interpreter is compiled into this file, so exec.h must not stand in for
// anything it already has
#define SHIBE_EXEC_INLINED

#define SHIBE_VM_EXECUTE shibe_execute_without_hook
#define SHIBE_HAS_HOOK 0
#include "exec.h"

#undef SHIBE_HAS_HOOK
#undef SHIBE_VM_EXECUTE

#define SHIBE_VM_EXECUTE shibe_execute_with_hook
#define SHIBE_HAS_HOOK 1
#include "exec.h"

#define BSEG_REALLOC(ptr, size, ctx) shibe_realloc(ptr, size, ctx)
#define BSEG_IMPLEMENTATION
#include <bseg.h>
