#include <btest.h>
#include <barena.h>
#include <shibe.h>
#include <shibe/alloc.h>
#include <bmacro.h>

static barena_pool_t arena_pool;
static barena_t arena;
static shibe_barena_t shibe_allocator;
static shibe_vm_t* vm;

static inline void
init_per_suite(void) {
	barena_pool_init(&arena_pool, 1);
}

static inline void
cleanup_per_suite(void) {
	barena_pool_cleanup(&arena_pool);
	// Drop the pointer so leak detection work
	arena_pool = (barena_pool_t){ 0 };
}

static inline void
init_per_test(void) {
	barena_init(&arena, &arena_pool);
	vm = shibe_create((shibe_config_t){
		.alloc = shibe_barena_init(&shibe_allocator, &arena_pool),
	});
}

static inline void
cleanup_per_test(void) {
	shibe_destroy(vm);
	barena_reset(&arena);
	// Do not reset the vm arena, the vn is supposed to reset it

	// Drop the arena so leak detection works
	arena = (barena_t){ 0 };
	shibe_allocator.arena = (barena_t){ 0 };
}
