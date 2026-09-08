#include <shibe.h>

#define SHIBE_REGION_BITS  3
#define SHIBE_INDEX_BITS   29
#define SHIBE_INDEX_MASK   ((1u << SHIBE_INDEX_BITS) - 1)
#define SHIBE_ZERO ((shibe_cell_t){ 0 })

#define BSEG_API static inline
// The first 3 bits are used to address a region.
// So the region-local has a 29 bit address space.
// Number of doubling segments that fit in the 29-bit index space.
// Segment 0 is 2^BSEG_SKIPPED_SEGMENTS elements, so:
// BSEG_MAX_SEGMENTS = 29 - BSEG_SKIPPED_SEGMENTS
#define BSEG_MAX_SEGMENTS 23
#include <bseg.h>
#define SHIBE_REGION_MAX_LEN (((size_t)1u << 29) - ((size_t)1u << 6))

typedef bseg(shibe_cell_t) shibe_mem_seg_t;

struct shibe_vm_s {
	shibe_config_t config;
	shibe_state_t state;

	shibe_mem_seg_t regions[8];

	void* snapshot;
};

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

static inline uint32_t
shibe_mem_index(shibe_cell_t addr) {
	return addr.u32 & SHIBE_INDEX_MASK;
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
	vm->state.tp =
	vm->state.tm = SHIBE_ZERO;

	vm->state.exec_state = SHIBE_EXEC_IDLE;
}

shibe_cell_t
shibe_alloc(shibe_vm_t* vm, shibe_mem_region_t region, shibe_cell_t num_cells) {
	if (num_cells.i32 < 0) {
		shibe_panic(vm, &(shibe_panic_t){
			.error = SHIBE_ERR_INVALID,
			.arg = num_cells,
		});
		return SHIBE_ZERO;
	}

	region = region & 0x07;
	shibe_mem_seg_t* mem_seg = &vm->regions[region];
	size_t len = bseg_len(*mem_seg);
	size_t new_len = len + (size_t)num_cells.u32;
	if (new_len <= SHIBE_REGION_MAX_LEN) {
		bseg_resize(*mem_seg, new_len, vm);
		return (shibe_cell_t){ .u32 = ((uint32_t)region << SHIBE_INDEX_BITS) | (uint32_t)len };
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

void
shibe_copy_to_vm  (shibe_vm_t* vm, shibe_cell_t vm_addr, const shibe_cell_t* host_addr, uint32_t num_cells) {
	for (uint32_t i = 0; i < num_cells && !shibe_panicked(vm); ++i) {
		shibe_store(vm, (shibe_cell_t){ vm_addr.u32 + i}, host_addr[i]);
	}
}

void
shibe_copy_to_host(shibe_vm_t* vm, shibe_cell_t vm_addr,       shibe_cell_t* host_addr, uint32_t num_cells) {
	for (uint32_t i = 0; i < num_cells && !shibe_panicked(vm); ++i) {
		shibe_cell_t value = shibe_fetch(vm, (shibe_cell_t){ vm_addr.u32 + i});
		if (shibe_panicked(vm)) { break; }
		host_addr[i] = value;
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

#define BSEG_REALLOC(ctx, ptr, size) shibe_realloc(ctx, ptr, size)
#define BSEG_IMPLEMENTATION
#include <bseg.h>
