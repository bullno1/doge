#include <shibe/asm.h>
#include <string.h>

// MSVC does not declare max_align_t in C mode. Its widest fundamental type is
// 8 bytes, so double is the same alignment; barena makes the same substitution.
#ifndef SHIBE_ASM_MAX_ALIGN_TYPE
#	ifdef _MSC_VER
#		define SHIBE_ASM_MAX_ALIGN_TYPE double
#	else
#		define SHIBE_ASM_MAX_ALIGN_TYPE max_align_t
#	endif
#endif

#define SHIBE_ASM_BUNDLE_LEN sizeof(shibe_bundle_t)

#define BSEG_API static inline
// Code is addressed by the same 29-bit region index as vm memory, so the
// buffer never usefully outgrows one region and the vm's segment count covers
// it. Segment 0 is 2^BSEG_SKIPPED_SEGMENTS elements, so:
// BSEG_MAX_SEGMENTS = 29 - BSEG_SKIPPED_SEGMENTS
#define BSEG_MAX_SEGMENTS 23
#include <bseg.h>

typedef struct {
	shibe_cell_t addr;
	bool bound;
} shibe_asm_label_info_t;

// A cell in the code buffer that will hold the address of a label
typedef struct {
	uint32_t index;
	uint32_t label;
} shibe_asm_fixup_t;

typedef bseg(shibe_cell_t) shibe_asm_code_t;
typedef bseg(shibe_asm_label_info_t) shibe_asm_label_seg_t;
typedef bseg(shibe_asm_fixup_t) shibe_asm_fixup_seg_t;

struct shibe_asm_s {
	shibe_vm_t* vm;
	shibe_allocator_t* allocator;
	void* snapshot;
	shibe_cell_t start_addr;

	// Assembled cells. `cursor_next` doubles as the length: both write heads
	// stay behind it.
	shibe_asm_code_t code;

	// Labels are addressed by a 1-based id so that 0 is SHIBE_ASM_NO_LABEL
	shibe_asm_label_seg_t labels;
	shibe_asm_fixup_seg_t fixups;

	// Opcode head: the bundle being filled and the next free slot in it.
	// `offset == SHIBE_ASM_BUNDLE_LEN` means no bundle is open.
	shibe_bundle_t bundle;
	uint32_t cursor;
	uint8_t offset;

	// Operand head: where the next immediate or data cell lands
	uint32_t cursor_next;

	// Sticky. Once set, every entry point becomes a no-op and shibe_asm_end
	// writes nothing.
	bool failed;
};

static inline void*
shibe_asm_realloc(void* ptr, size_t size, shibe_asm_t* sasm) {
	// bseg only ever asks for fresh segments; the old ones are reclaimed
	// wholesale by the restore in shibe_asm_end
	(void)ptr;
	if (size == 0) { return NULL; }

	shibe_allocator_t* allocator = sasm->allocator;
	// The callback carries no element type, so align for the worst case
	void* mem = allocator->alloc(allocator, size, _Alignof(SHIBE_ASM_MAX_ALIGN_TYPE));
	if (mem == NULL) { sasm->failed = true; }

	return mem;
}

static inline bool
shibe_asm_panicked(shibe_vm_t* vm) {
	return shibe_inspect(vm)->exec_state == SHIBE_EXEC_PANIC;
}

// bseg does nothing when it cannot grow, so the length it ends up with is the
// honest answer: an allocator refusing and BSEG_MAX_SEGMENTS being reached both
// land here. Nothing touches the array again once `failed` is set.
static bool
shibe_asm_reserve_code(shibe_asm_t* sasm, uint32_t num_cells) {
	// New cells are zeroed, which is what a data cell wants to start as
	bseg_resize(sasm->code, num_cells, sasm);
	if (bseg_len(sasm->code) < num_cells) {
		sasm->failed = true;
		return false;
	}

	return true;
}

// Append a cell at the operand head and return where it landed. The index is
// only meaningful when `failed` is still clear.
static uint32_t
shibe_asm_push_cell(shibe_asm_t* sasm, shibe_cell_t value) {
	uint32_t index = sasm->cursor_next;
	if (!shibe_asm_reserve_code(sasm, index + 1)) { return 0; }

	bseg_at(sasm->code, index) = value;
	sasm->cursor_next = index + 1;
	return index;
}

// Start a fresh bundle after every operand cell the previous one accumulated.
// Unused slots are NOP from the outset, so shibe_asm_align a matter of simply
// closing the bundle.
static void
shibe_asm_open_bundle(shibe_asm_t* sasm) {
	uint32_t cursor = sasm->cursor_next;
	if (!shibe_asm_reserve_code(sasm, cursor + 1)) { return; }

	memset(sasm->bundle, SHIBE_OP_NOP, sizeof(sasm->bundle));
	sasm->cursor = cursor;
	sasm->cursor_next = cursor + 1;
	sasm->offset = 0;
	bseg_at(sasm->code, cursor) = shibe_pack(sasm->bundle);
}

static bool
shibe_asm_valid_label(shibe_asm_t* sasm, shibe_asm_label_t label) {
	if (sasm->failed) { return false; }

	if (label.id == 0 || label.id > bseg_len(sasm->labels)) {
		sasm->failed = true;
		return false;
	}

	return true;
}

static void
shibe_asm_add_fixup(shibe_asm_t* sasm, uint32_t index, uint32_t label) {
	size_t num_fixups = bseg_len(sasm->fixups);
	bseg_push(sasm->fixups, ((shibe_asm_fixup_t){
		.index = index,
		.label = label,
	}), sasm);

	if (bseg_len(sasm->fixups) == num_fixups) { sasm->failed = true; }
}

shibe_asm_t*
shibe_asm_begin(shibe_vm_t* vm, shibe_allocator_t* allocator, shibe_cell_t start_addr) {
	void* snapshot = allocator->snapshot(allocator);
	shibe_asm_t* sasm = allocator->alloc(
		allocator, sizeof(shibe_asm_t), _Alignof(shibe_asm_t)
	);
	if (sasm == NULL) {
		allocator->restore(allocator, snapshot);
		return NULL;
	}

	// Zeroed bseg arrays are valid empty ones
	*sasm = (shibe_asm_t){
		.vm = vm,
		.allocator = allocator,
		.snapshot = snapshot,
		.start_addr = start_addr,
		// No bundle is open yet
		.offset = SHIBE_ASM_BUNDLE_LEN,
	};

	return sasm;
}

bool
shibe_asm_end(shibe_asm_t* sasm) {
	// Read out what outlives the assembler before the restore below frees it
	shibe_allocator_t* allocator = sasm->allocator;
	void* snapshot = sasm->snapshot;
	shibe_vm_t* vm = sasm->vm;
	uint32_t start = sasm->start_addr.u32;
	uint32_t len = sasm->cursor_next;

	bool ok = !sasm->failed;

	// Whatever is left of the open bundle traps instead of running on
	if (ok && sasm->offset < SHIBE_ASM_BUNDLE_LEN) {
		for (size_t i = sasm->offset; i < SHIBE_ASM_BUNDLE_LEN; ++i) {
			sasm->bundle[i] = SHIBE_OP_TRAP;
		}
		bseg_at(sasm->code, sasm->cursor) = shibe_pack(sasm->bundle);
		sasm->offset = SHIBE_ASM_BUNDLE_LEN;
	}

	// Every reference needs a home before any of this reaches the vm
	size_t num_fixups = ok ? bseg_len(sasm->fixups) : 0;
	for (size_t i = 0; ok && i < num_fixups; ++i) {
		ok = bseg_ref(sasm->labels, bseg_at(sasm->fixups, i).label - 1)->bound;
	}

	if (ok) {
		for (size_t i = 0; i < num_fixups; ++i) {
			shibe_asm_fixup_t fixup = bseg_at(sasm->fixups, i);
			bseg_at(sasm->code, fixup.index) =
				bseg_ref(sasm->labels, fixup.label - 1)->addr;
		}
	}

	// Running off the end of a region wraps into the next one, which would be
	// written silently rather than faulting
	if (ok && len > 0) {
		ok = shibe_mem_region((shibe_cell_t){ .u32 = start + len - 1 })
			== shibe_mem_region(sasm->start_addr);
	}

	if (ok) {
		for (uint32_t i = 0; i < len; ++i) {
			shibe_store(
				vm, (shibe_cell_t){ .u32 = start + i }, bseg_at(sasm->code, i)
			);
			if (shibe_asm_panicked(vm)) { ok = false; break; }
		}
	}

	allocator->restore(allocator, snapshot);
	return ok;
}

shibe_cell_t
shibe_asm_here(const shibe_asm_t* sasm) {
	// With a bundle open the next opcode joins it; otherwise it starts a new
	// one after any operand cells already queued
	uint32_t index = sasm->offset < SHIBE_ASM_BUNDLE_LEN
		? sasm->cursor
		: sasm->cursor_next;
	return (shibe_cell_t){ .u32 = sasm->start_addr.u32 + index };
}

// Writes the opcode into the open bundle without checking its operand, which
// the callers below have already done
static void
shibe_asm_emit_op(shibe_asm_t* sasm, shibe_opcode_t opcode) {
	if (sasm->offset >= SHIBE_ASM_BUNDLE_LEN) {
		shibe_asm_open_bundle(sasm);
		if (sasm->failed) { return; }
	}

	sasm->bundle[sasm->offset++] = opcode;
	bseg_at(sasm->code, sasm->cursor) = shibe_pack(sasm->bundle);

	// Nothing may share a bundle with a control transfer: the cell after this
	// bundle's operands has to be the next instruction. The slots left behind
	// keep the NOP they were opened with rather than becoming TRAP, so a
	// conditional branch that is not taken can run through them.
	if (shibe_opcode_flags(opcode) & SHIBE_OPCODE_FLAG_ENDS_BUNDLE) {
		sasm->offset = SHIBE_ASM_BUNDLE_LEN;
	}
}

void
shibe_asm_emit(shibe_asm_t* sasm, shibe_opcode_t opcode) {
	if (sasm->failed) { return; }

	// Without its operand cell the vm would take whatever follows as one
	if (shibe_opcode_flags(opcode) & SHIBE_OPCODE_FLAG_IMM) {
		sasm->failed = true;
		return;
	}

	shibe_asm_emit_op(sasm, opcode);
}

void
shibe_asm_emit_imm(shibe_asm_t* sasm, shibe_opcode_t opcode, shibe_cell_t operand) {
	if (sasm->failed) { return; }

	// The vm would never consume the cell and would run it as a bundle
	if (!(shibe_opcode_flags(opcode) & SHIBE_OPCODE_FLAG_IMM)) {
		sasm->failed = true;
		return;
	}

	shibe_asm_emit_op(sasm, opcode);
	if (sasm->failed) { return; }

	shibe_asm_push_cell(sasm, operand);
}

void
shibe_asm_emit_imm_label(shibe_asm_t* sasm, shibe_opcode_t opcode, shibe_asm_label_t label) {
	if (!shibe_asm_valid_label(sasm, label)) { return; }

	if (!(shibe_opcode_flags(opcode) & SHIBE_OPCODE_FLAG_IMM)) {
		sasm->failed = true;
		return;
	}

	shibe_asm_emit_op(sasm, opcode);
	if (sasm->failed) { return; }

	uint32_t index = shibe_asm_push_cell(sasm, (shibe_cell_t){ 0 });
	if (sasm->failed) { return; }

	shibe_asm_add_fixup(sasm, index, label.id);
}

void
shibe_asm_align(shibe_asm_t* sasm) {
	if (sasm->failed) { return; }

	// The slots left over were filled with NOP when the bundle was opened, so
	// closing it is the whole of the padding
	sasm->offset = SHIBE_ASM_BUNDLE_LEN;
}

void
shibe_asm_data(shibe_asm_t* sasm, shibe_cell_t value) {
	shibe_asm_align(sasm);
	if (sasm->failed) { return; }

	shibe_asm_push_cell(sasm, value);
}

void
shibe_asm_data_label(shibe_asm_t* sasm, shibe_asm_label_t label) {
	if (!shibe_asm_valid_label(sasm, label)) { return; }

	shibe_asm_align(sasm);
	if (sasm->failed) { return; }

	uint32_t index = shibe_asm_push_cell(sasm, (shibe_cell_t){ 0 });
	if (sasm->failed) { return; }

	shibe_asm_add_fixup(sasm, index, label.id);
}

shibe_asm_label_t
shibe_asm_make_label(shibe_asm_t* sasm) {
	if (sasm->failed) { return SHIBE_ASM_NO_LABEL; }

	size_t num_labels = bseg_len(sasm->labels);
	bseg_push(sasm->labels, ((shibe_asm_label_info_t){ 0 }), sasm);
	if (bseg_len(sasm->labels) == num_labels) {
		sasm->failed = true;
		return SHIBE_ASM_NO_LABEL;
	}

	return (shibe_asm_label_t){ .id = (uint32_t)(num_labels + 1) };
}

void
shibe_asm_bind_label(shibe_asm_t* sasm, shibe_asm_label_t label) {
	if (!shibe_asm_valid_label(sasm, label)) { return; }

	shibe_asm_label_info_t* info = bseg_ref(sasm->labels, label.id - 1);
	if (info->bound) {
		// Bound twice: the second address would silently win
		sasm->failed = true;
		return;
	}

	shibe_asm_align(sasm);
	if (sasm->failed) { return; }

	info->addr = shibe_asm_here(sasm);
	info->bound = true;
}

void
shibe_asm_bind_value(shibe_asm_t* sasm, shibe_asm_label_t label, shibe_cell_t value) {
	if (!shibe_asm_valid_label(sasm, label)) { return; }

	shibe_asm_label_info_t* info = bseg_ref(sasm->labels, label.id - 1);
	if (info->bound) {
		// Bound twice: the second value would silently win
		sasm->failed = true;
		return;
	}

	// No alignment: unlike a code label this does not name a location
	info->addr = value;
	info->bound = true;
}

#define BSEG_REALLOC(ptr, size, ctx) shibe_asm_realloc(ptr, size, ctx)
#define BSEG_IMPLEMENTATION
#include <bseg.h>
