#include <shibe/alloc.h>
#include <bmacro.h>

static void*
shibe_barena_memalign(void* ctx, size_t size, size_t alignment) {
	return barena_memalign(
		&BCONTAINER_OF(ctx, shibe_barena_t, base)->arena,
		size,
		alignment
	);
}

static void*
shibe_barena_snapshot(void* ctx) {
	return barena_snapshot(&BCONTAINER_OF(ctx, shibe_barena_t, base)->arena);
}

static void
shibe_barena_restore(void* ctx, void* snapshot) {
	barena_restore(&BCONTAINER_OF(ctx, shibe_barena_t, base)->arena, snapshot);
}

shibe_alloc_t*
shibe_barena_init(shibe_barena_t* alloc, barena_pool_t* pool) {
	barena_init(&alloc->arena, pool);
	alloc->base = (shibe_alloc_t){
		.alloc = shibe_barena_memalign,
		.restore = shibe_barena_restore,
		.snapshot = shibe_barena_snapshot,
	};
	return &alloc->base;
}
