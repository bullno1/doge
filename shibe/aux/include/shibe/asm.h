#ifndef SHIBE_ASM_H
#define SHIBE_ASM_H

#include <shibe.h>
#include <shibe/opcode.h>

/**
 * Assembler for shibe VM
 *
 * Shibe is a bytecode VM with memory organized into 32-bit cells.
 * However, its opcodes are 8 bit, packed into groups of 4 within a cell,
 * called "bundle".
 * The ip (instruction pointer) register always points at a cell which is the
 * next cell to be executed.
 * The VM executes 4 instructions per bundle.
 *
 * Most opcodes take operands from the stack.
 * However, some (LIT, JMP, AGET, EXTCALL...) take an operand from the
 * instruction stream itself, known here as an immediate.
 * Immediates do not sit inside the bundle: they are consumed from the cells
 * following it, in the order their opcodes appear in the bundle, and the ip is
 * advanced past each one.
 * A bundle can therefore carry up to 4 immediate-taking opcodes, followed by
 * their 4 operand cells.
 *
 * Jump targets can only be at cell boundary.
 * Thus, sometimes the bundle is filled with NOP for alignment.
 */

/*! An assembler instance, created by @ref shibe_asm_begin */
typedef struct shibe_asm_s shibe_asm_t;

/*! An opaque label id, only meaningful to the assembler that made it */
typedef struct { uint32_t id; } shibe_asm_label_t;

/*! The id no label ever has */
#define SHIBE_ASM_NO_LABEL ((shibe_asm_label_t){ .id = 0 })

/**
 * Create an assembler writing into `vm` at `start_addr`
 *
 * Code is assembled into a host-side buffer and only written into VM memory by
 * @ref shibe_asm_end, so a run that fails while assembling leaves the VM
 * untouched.
 * The cells at `start_addr` need not be allocated until @ref shibe_asm_end.
 *
 * `allocator` backs the buffer and the assembler's own bookkeeping, never VM
 * memory, and must belong to the assembler alone: it is snapshotted here
 * and restored by@ref shibe_asm_end, which frees anything else taken from it
 * in the meantime.
 *
 * Returns NULL if the assembler could not be allocated.
 */
shibe_asm_t*
shibe_asm_begin(shibe_vm_t* vm, shibe_allocator_t* allocator, shibe_cell_t start_addr);

/**
 * Finish assembling: resolve every forward reference, pad the trailing bundle
 * with TRAP, write the code into the VM at `start_addr`, then destroy the
 * assembler.
 *
 * Trailing padding is TRAP rather than NOP so that running off the end of the
 * code faults instead of drifting into whatever follows.
 *
 * Returns false if the code could not be laid down: a label referenced but
 * never bound, a label bound twice or never made, an allocation that failed
 * along the way, code that would cross out of the region it starts in, or a
 * faulting write.
 * Except for a faulting write, every other failure leaves the VM exactly as it
 * was.
 *
 * Either way `sasm` is destroyed and the allocator is restored to its state at
 * @ref shibe_asm_begin, so the pointer must not be used again.
 */
bool
shibe_asm_end(shibe_asm_t* sasm);

/**
 * The address the next opcode would be written to
 *
 * Only a cell boundary right after @ref shibe_asm_align or
 * @ref shibe_asm_bind_label.
 */
shibe_cell_t
shibe_asm_here(const shibe_asm_t* sasm);

/*! Emit an opcode that takes no immediate */
void
shibe_asm_emit(shibe_asm_t* sasm, shibe_opcode_t opcode);

/**
 * Emit an opcode along with its immediate
 *
 * For every opcode whose operand comes from the instruction stream: LIT, AGET,
 * ASET, EXTCALL and the like.
 * Use @ref shibe_asm_emit_imm_label when the operand is a code address.
 */
void
shibe_asm_emit_imm(shibe_asm_t* sasm, shibe_opcode_t opcode, shibe_cell_t operand);

/**
 * Emit an opcode whose immediate is the address of `label`
 *
 * For JMP, JZ, CALL and anything else branching to a label.
 * The label may still be unbound; the reference is patched by
 * @ref shibe_asm_end.
 */
void
shibe_asm_emit_imm_label(shibe_asm_t* sasm, shibe_opcode_t opcode, shibe_asm_label_t label);

/**
 * Align the write head to cell boundary
 *
 * Padding here is NOP, not TRAP: control falls through it into whatever is
 * aligned next.
 */
void
shibe_asm_align(shibe_asm_t* sasm);

/*! Emit a raw cell of data at the write head, aligning first */
void
shibe_asm_data(shibe_asm_t* sasm, shibe_cell_t value);

/**
 * Emit a raw cell holding the address of `label`, aligning first
 *
 * The data counterpart of @ref shibe_asm_emit_imm_label, for jump tables and
 * the like. The label may still be unbound.
 */
void
shibe_asm_data_label(shibe_asm_t* sasm, shibe_asm_label_t label);

/*! Create a label, initially unbound */
shibe_asm_label_t
shibe_asm_make_label(shibe_asm_t* sasm);

/**
 * Bind `label` to the current address, aligning first
 *
 * A label must be bound exactly once, at or before @ref shibe_asm_end.
 */
void
shibe_asm_bind_label(shibe_asm_t* sasm, shibe_asm_label_t label);

#endif
