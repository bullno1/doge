#ifndef SHIBE_STR_H
#define SHIBE_STR_H

#include <shibe.h>

// A string starts on a cell boundary and is laid out as:
//
//   [len] [ byte 0 .. byte len-1, '\0', padding ]
//
// The first cell holds the length in bytes, not counting the terminator.
// The payload follows in the next cells, zero-terminated so it can be handed to
// C code as-is, and zero-padded to the end of the last cell.

// Number of cells a string of `len` bytes occupies, including the length cell
static inline uint32_t
shibe_str_num_cells(uint32_t len) {
	return 1 + (len + sizeof(shibe_cell_t)) / sizeof(shibe_cell_t);
}

// Address of the payload of the string at `addr`
static inline shibe_cell_t
shibe_str_payload(shibe_cell_t addr) {
	return (shibe_cell_t){ .u32 = addr.u32 + 1 };
}

// Allocate a string in `region` and copy `len` bytes of `str` into it.
// Returns the address of the length cell.
shibe_cell_t
shibe_str_alloc(shibe_vm_t* vm, shibe_mem_region_t region, const char* str, uint32_t len);

// Length in bytes of the string at `addr`, not counting the terminator
uint32_t
shibe_str_len(shibe_vm_t* vm, shibe_cell_t addr);

// Copy the string at `addr` into `buf`, which is always terminated when
// `buf_size` is non-zero. Returns the full length of the string, so a result
// >= buf_size means the copy was truncated.
uint32_t
shibe_str_copy(shibe_vm_t* vm, shibe_cell_t addr, char* buf, uint32_t buf_size);

#endif
