#include <shibe/alloc.h>
#include <bmacro.h>

static void*
shibe_barena_memalign(shibe_allocator_t* ctx, size_t size, size_t alignment) {
	return barena_memalign(
		&BCONTAINER_OF(ctx, shibe_barena_t, impl)->arena,
		size,
		alignment
	);
}

static void*
shibe_barena_snapshot(shibe_allocator_t* ctx) {
	return barena_snapshot(&BCONTAINER_OF(ctx, shibe_barena_t, impl)->arena);
}

static void
shibe_barena_restore(shibe_allocator_t* ctx, void* snapshot) {
	barena_restore(&BCONTAINER_OF(ctx, shibe_barena_t, impl)->arena, snapshot);
}

shibe_allocator_t*
shibe_barena_init(shibe_barena_t* alloc, barena_pool_t* pool) {
	barena_init(&alloc->arena, pool);
	alloc->impl = (shibe_allocator_t){
		.alloc = shibe_barena_memalign,
		.restore = shibe_barena_restore,
		.snapshot = shibe_barena_snapshot,
	};
	return &alloc->impl;
}
