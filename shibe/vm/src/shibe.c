#include <shibe.h>
#define BSEG_API static inline
#include <bseg.h>

struct shibe_vm_s {
	shibe_config_t config;
	shibe_state_t state;

	bseg(shibe_cell_t) banks[8];

	void* snapshot;
};

static inline void*
shibe_memalign(shibe_alloc_t* alloc, size_t size, size_t alignment) {
	return alloc->alloc(alloc, size, alignment);
}

static inline void*
shibe_snapshot(shibe_alloc_t* alloc) {
	return alloc->snapshot(alloc);
}

static inline void
shibe_restore(shibe_alloc_t* alloc, void* snapshot) {
	alloc->restore(alloc, snapshot);
}

static inline void*
shibe_realloc(void* ptr, size_t size, shibe_alloc_t* alloc) {
	if (size != 0) {
		return shibe_memalign(alloc, size, _Alignof(shibe_cell_t));
	} else {
		return NULL;
	}
}

shibe_vm_t*
shibe_create(shibe_config_t config) {
	void* snapshot = shibe_snapshot(config.alloc);
	shibe_vm_t* vm = shibe_memalign(config.alloc, sizeof(shibe_vm_t), _Alignof(shibe_vm_t));
	*vm = (shibe_vm_t){
		.config = config,
		.snapshot = snapshot,
	};

	// Address 0 is special
	bseg_resize(vm->banks[0], 1, config.alloc);

	return vm;
}

void
shibe_destroy(shibe_vm_t* vm) {
	shibe_restore(vm->config.alloc, vm->snapshot);
}

#define BSEG_REALLOC(ctx, ptr, size) shibe_realloc(ctx, ptr, size)
#define BSEG_IMPLEMENTATION
#include <bseg.h>
