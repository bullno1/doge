#ifndef SHIBE_INTERNAL_H
#define SHIBE_INTERNAL_H

#include <shibe.h>

struct shibe_vm_s {
	shibe_config_t config;
	shibe_state_t state;

	void* snapshot;
};

static inline void*
shibe_alloc(const shibe_alloc_t* alloc, size_t size, size_t alignment) {
	return alloc->alloc(alloc->ctx, size, alignment);
}

static inline void*
shibe_snapshot(const shibe_alloc_t* alloc) {
	return alloc->snapshot(alloc->ctx);
}

static inline void
shibe_restore(const shibe_alloc_t* alloc, void* snapshot) {
	alloc->restore(alloc->ctx, snapshot);
}

#endif
