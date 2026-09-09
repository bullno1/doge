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
shibe_execute_with_hook(shibe_vm_t* vm);

static shibe_status_t
shibe_execute_without_hook(shibe_vm_t* vm);

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
	// single activation, so the caller's are put back below.
	shibe_cell_t caller_ip = vm->state.ip;
	shibe_cell_t caller_fp = vm->state.fp;
	uint32_t caller_asp = vm->state.asp.u32;
	bool nested = caller_state == SHIBE_EXEC_RUNNING;

	if (nested) {
		// Record the boundary on the auxiliary stack so a stack walker can
		// cross it. The caller's `ip` goes where a CALL would have left its
		// return address, under a header whose creator is 0.
		if (vm->config.as_len - caller_asp < SHIBE_AUX_REENTRY_LEN) {
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_STACK_OVERFLOW,
				.arg = SHIBE_STACK_AS,
			});
			return SHIBE_ERROR;
		}

		shibe_cell_t* as = vm->state.as;
		as[caller_asp + 0] = caller_ip;
		as[caller_asp + 1] = vm->state.dsp;
		as[caller_asp + 2] = caller_fp;
		as[caller_asp + 3] = vm->state.tm;
		// Nothing else can produce this: address 0 is reserved, so no ENTER
		// operand cell ever lives there
		as[caller_asp + 4] = SHIBE_ZERO;

		vm->state.fp.u32 = caller_asp + SHIBE_AUX_REENTRY_LEN;
		vm->state.asp.u32 = caller_asp + SHIBE_AUX_REENTRY_LEN;
	}

	vm->state.ip = addr;
	vm->state.exec_state = SHIBE_EXEC_RUNNING;

	// Creating 2 separate versions is the only way to have optimized opcode
	// dispatch when no debug hook is attached
	shibe_status_t status;
	if (vm->config.host->debug == NULL) {
		status = shibe_execute_without_hook(vm);
	} else {
		status = shibe_execute_with_hook(vm);
	}

	// Hand the outer run its activation back. A panic or a suspension is left
	// where it stopped: it ends the whole nest, there is nowhere to record an
	// outer activation for a resume to come back to, and leaving the boundary
	// frame in place is what lets the host walk the stack afterwards.
	if (nested && status == SHIBE_OK) {
		// The entry point has to be a stub that halts, so it must give the
		// auxiliary stack back exactly as it found it
		if (vm->state.asp.u32 != caller_asp + SHIBE_AUX_REENTRY_LEN
		 || vm->state.fp.u32 != caller_asp + SHIBE_AUX_REENTRY_LEN) {
			shibe_panic(vm, &(shibe_panic_t){
				.error = SHIBE_ERR_INVALID,
			});
			return SHIBE_ERROR;
		}

		vm->state.asp.u32 = caller_asp;
		vm->state.fp = caller_fp;
		vm->state.ip = caller_ip;
		vm->state.exec_state = SHIBE_EXEC_RUNNING;
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
