#include <stdbool.h>
#include <shibe/str.h>

static inline bool
shibe_str_panicked(shibe_vm_t* vm) {
	return shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC;
}

shibe_cell_t
shibe_str_alloc(shibe_vm_t* vm, shibe_mem_region_t region, const char* str, uint32_t len) {
	shibe_cell_t addr = shibe_alloc(
		vm, region, (shibe_cell_t){ .u32 = shibe_str_num_cells(len) }
	);
	if (shibe_str_panicked(vm)) { return (shibe_cell_t){ 0 }; }

	shibe_store(vm, addr, (shibe_cell_t){ .u32 = len });
	// Fresh cells are zeroed so the terminator and the padding are already there
	shibe_copy_to_vm(vm, shibe_str_payload(addr), str, len);

	return addr;
}

uint32_t
shibe_str_len(shibe_vm_t* vm, shibe_cell_t addr) {
	return shibe_fetch(vm, addr).u32;
}

uint32_t
shibe_str_copy(shibe_vm_t* vm, shibe_cell_t addr, char* buf, uint32_t buf_size) {
	uint32_t len = shibe_fetch(vm, addr).u32;
	if (shibe_str_panicked(vm)) { return 0; }

	if (buf_size == 0) { return len; }

	uint32_t num_bytes = len < buf_size ? len : buf_size - 1;
	shibe_copy_to_host(vm, shibe_str_payload(addr), buf, num_bytes);
	if (shibe_str_panicked(vm)) { return 0; }

	buf[num_bytes] = '\0';
	return len;
}
