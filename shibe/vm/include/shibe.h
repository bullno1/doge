#ifndef SHIBE_H
#define SHIBE_H

#include <stdint.h>
#include <stddef.h>

#ifndef SHIBE_API
#define SHIBE_API
#endif

typedef struct shibe_vm_s shibe_vm_t;

typedef union {
	int32_t i32;
	uint32_t u32;
	float f32;
} shibe_cell_t;

typedef enum {
	SHIBE_OK,
	SHIBE_SUSPENDED,
	SHIBE_ERROR,
} shibe_status_t;

typedef enum {
	SHIBE_EXEC_IDLE,
	SHIBE_EXEC_RUNNING,
	SHIBE_EXEC_SUSPENDED,
	SHIBE_EXEC_PANIC,
} shibe_exec_state_t;

typedef enum {
	SHIBE_ERR_NONE,
	SHIBE_ERR_INVALID,
	SHIBE_ERR_STACK_OVERFLOW,
	SHIBE_ERR_STACK_UNDERFLOW,
	SHIBE_ERR_OOM,
	SHIBE_ERR_MEM_FAULT,
	SHIBE_ERR_TRAP,
	SHIBE_ERR_HOST,
} shibe_error_t;

typedef struct {
	shibe_error_t error;
	shibe_cell_t arg;
} shibe_panic_t;

typedef struct {
	// registers
	shibe_cell_t ip;
	shibe_cell_t dsp;
	shibe_cell_t asp;
	shibe_cell_t tp;
	shibe_cell_t tm;

	// stacks
	shibe_cell_t* ds;
	shibe_cell_t* as;

	shibe_exec_state_t exec_state;
} shibe_state_t;

typedef enum {
	SHIBE_MEM_REGION_0,
	SHIBE_MEM_REGION_1,
	SHIBE_MEM_REGION_2,
	SHIBE_MEM_REGION_3,
	SHIBE_MEM_REGION_4,
	SHIBE_MEM_REGION_5,
	SHIBE_MEM_REGION_6,
	SHIBE_MEM_REGION_7,
} shibe_mem_region_t;

typedef struct shibe_allocator_s shibe_allocator_t;
struct shibe_allocator_s {
	void* (*alloc)(shibe_allocator_t* allocator, size_t size, size_t alignment);
	void* (*snapshot)(shibe_allocator_t* allocator);
	void (*restore)(shibe_allocator_t* allocator, void* snapshot);
};

typedef struct shibe_host_s shibe_host_t;
struct shibe_host_s {
	void (*panic)(shibe_host_t* host, shibe_vm_t* vm, const shibe_panic_t* panic);
	shibe_status_t (*debug)(shibe_host_t* host, shibe_vm_t* vm, const shibe_state_t* state);
	shibe_status_t (*extcall)(shibe_host_t* host, shibe_vm_t* vm, shibe_cell_t index);
};

typedef struct {
	size_t ds_len;
	size_t as_len;

	shibe_allocator_t* allocator;
	shibe_host_t* host;
} shibe_config_t;

SHIBE_API shibe_vm_t*
shibe_create(shibe_config_t config);

SHIBE_API void
shibe_destroy(shibe_vm_t* vm);

SHIBE_API shibe_vm_t*
shibe_fork(shibe_vm_t* vm, const shibe_config_t* config);

SHIBE_API void
shibe_reset(shibe_vm_t* vm);

SHIBE_API shibe_cell_t
shibe_alloc(shibe_vm_t* vm, shibe_mem_region_t region, shibe_cell_t num_cells);

SHIBE_API shibe_cell_t
shibe_fetch(shibe_vm_t* vm, shibe_cell_t vm_addr);

SHIBE_API void
shibe_store(shibe_vm_t* vm, shibe_cell_t vm_addr, shibe_cell_t value);

SHIBE_API void
shibe_copy_to_vm  (shibe_vm_t* vm, shibe_cell_t vm_addr, const shibe_cell_t* host_addr, uint32_t num_cells);

SHIBE_API void
shibe_copy_to_host(shibe_vm_t* vm, shibe_cell_t vm_addr,       shibe_cell_t* host_addr, uint32_t num_cells);

SHIBE_API const shibe_state_t*
shibe_inspect(shibe_vm_t* vm);

SHIBE_API void
shibe_push(shibe_vm_t* vm, shibe_cell_t item);

SHIBE_API shibe_cell_t
shibe_pop(shibe_vm_t* vm);

SHIBE_API shibe_status_t
shibe_execute(shibe_vm_t* vm, shibe_cell_t addr);

SHIBE_API shibe_status_t
shibe_resume(shibe_vm_t* vm);

static inline shibe_mem_region_t
shibe_mem_region(shibe_cell_t addr) {
	return (shibe_mem_region_t)(addr.u32 >> 29);
}

#endif
