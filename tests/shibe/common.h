#include <btest.h>
#include <barena.h>
#include <shibe.h>

static barena_pool_t arena_pool;
static barena_t test_arena;
static barena_t vm_arena;

static void*
shibe_memalign(void* ctx, size_t size, size_t alignment) {
	return barena_memalign(ctx, size, alignment);
}

static void*
shibe_snapshot(void* ctx) {
	return barena_snapshot(ctx);
}

static void
shibe_restore(void* ctx, void* snapshot) {
	barena_restore(ctx, snapshot);
}

static shibe_alloc_t shibe_alloc = {
	.ctx = &vm_arena,
	.alloc = shibe_memalign,
	.snapshot = shibe_snapshot,
	.restore = shibe_restore,
};

static inline void
init_per_suite(void) {
	barena_pool_init(&arena_pool, 1);
}

static inline void
cleanup_per_suite(void) {
	barena_pool_cleanup(&arena_pool);
	arena_pool = (barena_pool_t){ 0 };
}

static inline void
init_per_test(void) {
	barena_init(&test_arena, &arena_pool);
	barena_init(&vm_arena, &arena_pool);
}

static inline void
cleanup_per_test(void) {
	barena_reset(&test_arena);
	// Do not reset the vm arena, the vn is supposed to reset it

	// Drop the arena so leak detection works
	test_arena = (barena_t){ 0 };
	vm_arena = (barena_t){ 0 };
}
