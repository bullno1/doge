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
	SHIBE_EXEC_ERROR,
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
	shibe_cell_t addr;
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
	SHIBE_MEM_BANK_0,
	SHIBE_MEM_BANK_1,
	SHIBE_MEM_BANK_2,
	SHIBE_MEM_BANK_3,
	SHIBE_MEM_BANK_4,
	SHIBE_MEM_BANK_5,
	SHIBE_MEM_BANK_6,
	SHIBE_MEM_BANK_7,
} shibe_mem_bank_t;

typedef struct {
	void* (*alloc)(void* ctx, size_t size, size_t alignment);
	void* (*snapshot)(void* ctx);
	void (*restore)(void* ctx, void* snapshot);
} shibe_alloc_t;

typedef struct {
	shibe_status_t (*debug)(void* ctx, shibe_vm_t* vm, const shibe_state_t* state);
	shibe_status_t (*panic)(void* ctx, shibe_vm_t* vm, const shibe_panic_t* panic);
	shibe_status_t (*extcall)(void* ctx, shibe_vm_t* vm, shibe_cell_t index);
} shibe_host_t;

typedef struct {
	size_t data_stack_size;
	size_t aux_stack_size;

	shibe_alloc_t* alloc;
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
shibe_fetch(shibe_vm_t* vm, shibe_cell_t vm_addr);

SHIBE_API void
shibe_store(shibe_vm_t* vm, shibe_cell_t vm_addr, shibe_cell_t value);

SHIBE_API shibe_cell_t
shibe_alloc(shibe_vm_t* vm, shibe_mem_bank_t bank, shibe_cell_t size);

SHIBE_API void
shibe_copy_to_vm  (shibe_vm_t* vm, shibe_cell_t vm_addr, const void* host_addr, size_t num_bytes);

SHIBE_API void
shibe_copy_to_host(shibe_vm_t* vm, shibe_cell_t vm_addr,       void* host_addr, size_t num_bytes);

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

#endif
