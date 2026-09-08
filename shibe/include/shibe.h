#ifndef SHIBE_H
#define SHIBE_H

#include <stdint.h>
#include <stddef.h>

#ifndef SHIBE_API
#define SHIBE_API
#endif

#define SHIBE_RESUME ((shibe_i32_t)-1)

typedef struct shibe_vm_s shibe_vm_t;
typedef int32_t shibe_i32_t;
typedef float shibe_f32_t;

typedef union {
	shibe_i32_t i32;
	shibe_f32_t f32;
} shibe_cell_t;

typedef enum {
	SHIBE_OK,
	SHIBE_ERROR,
	SHIBE_SUSPENDED,
} shibe_status_t;

typedef enum {
	SHIBE_EXEC_IDLE,
	SHIBE_EXEC_RUNNING,
	SHIBE_EXEC_SUSPENDED,
} shibe_exec_state_t;

typedef enum {
	SHIBE_ERR_NONE,
	SHIBE_ERR_INVALID,
	SHIBE_ERR_STACK_OVERFLOW,
	SHIBE_ERR_STACK_UNDERFLOW,
	SHIBE_ERR_OOM,
	SHIBE_ERR_MEM_FAULT,
	SHIBE_ERR_TRAP,
} shibe_error_t;

typedef struct {
	void* ctx;

	void* (*alloc)(void* ctx, size_t size, size_t alignment);
	void* (*snapshot)(void* ctx);
	void (*restore)(void* ctx, void* snapshot);
} shibe_alloc_t;

typedef struct {
	shibe_error_t error;
	shibe_cell_t addr;
} shibe_fault_t;

typedef struct {
	void* ctx;

	shibe_status_t (*fault)(void* ctx, shibe_vm_t* vm, const shibe_fault_t* fault);
	shibe_status_t (*extcall)(void* ctx, shibe_vm_t* vm, shibe_cell_t index);
} shibe_host_t;

typedef struct {
	size_t data_stack_size;
	size_t aux_stack_size;

	shibe_alloc_t alloc;
	shibe_host_t host;
} shibe_config_t;

typedef struct {
	// registers
	shibe_i32_t ip;
	shibe_i32_t dsp;
	shibe_i32_t asp;
	shibe_i32_t tp;
	shibe_i32_t tm;

	// stacks
	shibe_cell_t* ds;
	shibe_cell_t* as;

	shibe_exec_state_t exec_state;
} shibe_state_t;

SHIBE_API shibe_vm_t*
shibe_create(shibe_config_t config);

SHIBE_API void
shibe_destroy(shibe_vm_t* vm);

SHIBE_API shibe_vm_t*
shibe_fork(shibe_vm_t* vm, const shibe_config_t* config);

SHIBE_API shibe_cell_t
shibe_fetch(shibe_vm_t* vm, shibe_i32_t vm_addr);

SHIBE_API void
shibe_store(shibe_vm_t* vm, shibe_i32_t vm_addr, shibe_cell_t value);

SHIBE_API void
shibe_copy_to_vm  (shibe_vm_t* vm, shibe_i32_t vm_addr, const void* host_addr, size_t num_bytes);

SHIBE_API void
shibe_copy_to_host(shibe_vm_t* vm, shibe_i32_t vm_addr,       void* host_addr, size_t num_bytes);

SHIBE_API const shibe_state_t*
shibe_inspect(shibe_vm_t* vm);

SHIBE_API void
shibe_push(shibe_vm_t* vm, shibe_cell_t item);

SHIBE_API shibe_cell_t
shibe_pop(shibe_vm_t* vm);

SHIBE_API shibe_status_t
shibe_execute(shibe_vm_t* vm, shibe_i32_t addr);

#endif
