#include <btest.h>
#include <barena.h>
#include <shibe.h>
#include <shibe/alloc.h>
#include <bmacro.h>

#define TEST_DS_LEN 256u
// Small on purpose. The re-entrancy tests nest shibe_execute until this runs
// out, and every level costs a host call frame on the way: left at the default
// the auxiliary stack outlasts the host's own stack, which on wasm means v8
// gives up first and the test dies instead of panicking.
#define TEST_AS_LEN 64u

static barena_pool_t arena_pool;
static barena_t arena;
static shibe_barena_t shibe_allocator;
static shibe_vm_t* vm;

static shibe_panic_t last_panic;
static int num_panics;

static inline void
record_panic(shibe_host_t* host, shibe_vm_t* panicking_vm, const shibe_panic_t* panic) {
	(void)host;
	(void)panicking_vm;
	last_panic = *panic;
	++num_panics;
}

static shibe_host_t test_host = {
	.panic = record_panic,
};

// Acknowledge the last panic and bring the vm back to a usable state
static inline void
clear_panic(void) {
	last_panic = (shibe_panic_t){ 0 };
	num_panics = 0;
	shibe_reset(vm);
}

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
	last_panic = (shibe_panic_t){ 0 };
	num_panics = 0;
	vm = shibe_create((shibe_config_t){
		.ds_len = TEST_DS_LEN,
		.as_len = TEST_AS_LEN,
		.allocator = shibe_barena_init(&shibe_allocator, &arena_pool),
		.host = &test_host,
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
