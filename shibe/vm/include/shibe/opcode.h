#ifndef SHIBE_OPCODE_H
#define SHIBE_OPCODE_H

#include <stddef.h>
#include <shibe.h>

// Flags describing how an opcode interacts with the bundle it sits in.
// The assembler enforces them; the VM assumes them.
#define SHIBE_OPCODE_FLAG_NONE        0
// Consumes the operand cell that follows the bundle
#define SHIBE_OPCODE_FLAG_IMM         (1u << 0)
// No opcode may follow it in its bundle, so that the cell after the bundle's
// operands is the next instruction. The assembler closes the bundle after one,
// padding the unused slots with NOP. Every opcode that transfers control or
// suspends needs this, or there is no address to come back to. HALT carries it
// so that no unreachable slot is emitted after it. TRAP does not, because the
// assembler fills the leftover slots of a bundle with it.
#define SHIBE_OPCODE_FLAG_ENDS_BUNDLE (1u << 1)

// Each entry is: name, group, flags, stack effect, description.
//
// The stack effect is informal. The VM is untyped and the annotations only say
// how an opcode reads the cells it touches; type checking happens in the high
// level language. The form is `before -- after`, with `;` separating the data
// stack from the auxiliary stack and `..` standing for a variable number of
// cells. An operand cell taken from the instruction stream is not part of the
// effect - SHIBE_OPCODE_FLAG_IMM is what says the opcode has one.
#define SHIBE_OPCODE(X) \
	X(TRAP,     "Special", 0, "--", "Halt execution with SHIBE_ERR_TRAP") \
	X(NOP,      "Special", 0, "--", "Do nothing") \
	\
	X(JMP,      "Flow control", SHIBE_OPCODE_FLAG_IMM | SHIBE_OPCODE_FLAG_ENDS_BUNDLE, "--", "Continue at the operand address") \
	X(JZ,       "Flow control", SHIBE_OPCODE_FLAG_IMM | SHIBE_OPCODE_FLAG_ENDS_BUNDLE, "cond:u32 --", "Jump to the operand if `cond` value is 0") \
	X(CALL,     "Flow control", SHIBE_OPCODE_FLAG_ENDS_BUNDLE                        , "target:u32 -- ; -- ip:u32", "Push the return address (`ip`) to the auxiliary stack, jump to `target` address") \
	X(CCALL,    "Flow control", SHIBE_OPCODE_FLAG_ENDS_BUNDLE                        , "cond:u32 target:u32 -- ; -- ip:u32", "If `cond` is non-zero, push the return address and jump to `target` address. Regardless of `cond`, `target` is always consumed") \
	X(RET,      "Flow control", SHIBE_OPCODE_FLAG_ENDS_BUNDLE                        , "-- ; return:u32 --", "Return to an address in the auxiliary stack") \
	X(HALT,     "Flow control", SHIBE_OPCODE_FLAG_ENDS_BUNDLE                        , "--", "Halt execution and return to the host. Execution cannot be resumed") \
	\
	X(LIT,      "Stack manipulation", SHIBE_OPCODE_FLAG_IMM, "-- operand:cell", "Push the operand into the stack") \
	X(DUP,      "Stack manipulation", 0,                     "x:cell -- x x", "Duplicate the top of the stack") \
	X(DRP,      "Stack manipulation", 0,                     "x:cell --", "Drop the top value of the stack") \
	X(SWP,      "Stack manipulation", 0,                     "a:cell b:cell -- b a", "Swap the top 2 values of the stack") \
	X(ROT,      "Stack manipulation", 0,                     "a:cell b:cell c:cell -- b c a", "Rotate the top 3 values of the stack, bringing the third value to the top") \
	X(NIP,      "Stack manipulation", 0,                     "a:cell b:cell -- b", "Drop the second to top value of the stack") \
	X(OVR,      "Stack manipulation", 0,                     "a:cell b:cell -- a b a", "Duplicate the second to top value of the stack") \
	\
	X(FETCH,    "Memory", 0, "addr:u32 -- value:cell", "Load a cell at address `addr`") \
	X(STORE,    "Memory", 0, "value:cell addr:u32 --", "Store `value` at address `addr`") \
	X(BFETCH,   "Memory", 0, "addr:u32 offset:u32 -- value:byte", "Load a byte at address `addr` and offset `offset` within the cell. `offset` is masked to [0, 3] (`offset & 3`).") \
	X(BSTORE,   "Memory", 0, "value:byte addr:u32 offset:u32 --", "Store `value` at address `addr` and offset `offset` within the cell. `offset` is masked to [0, 3] (`offset & 3`) and `value` is masked to [0, 255] (`value & 0xff`).") \
	X(COPY,     "Memory", 0, "dst:u32 src:u32 len:u32 --", "Bulk copy `len` cells from `src` to `dst`. This is similar to `memmove` in C but works at cell granularity.") \
	\
	X(ADD,      "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs+rhs:u32", "Integer addition") \
	X(SUB,      "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs-rhs:u32", "Integer subtraction") \
	X(MUL,      "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs*rhs:u32", "Integer multiplication") \
	X(SDIV,     "Integer arithmetic", 0, "lhs:i32 rhs:i32 -- lhs/rhs:i32", "Signed integer division. A zero divisor yields 0, and `INT32_MIN / -1` wraps to `INT32_MIN`") \
	X(SREM,     "Integer arithmetic", 0, "lhs:i32 rhs:i32 -- lhs%rhs:i32", "Signed integer remainder. A zero divisor yields `lhs`, keeping `lhs == (lhs / rhs) * rhs + lhs % rhs` true for every input. `INT32_MIN % -1` yields 0") \
	X(UDIV,     "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs/rhs:u32", "Unsigned integer division. A zero divisor yields 0") \
	X(UREM,     "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs%rhs:u32", "Unsigned integer remainder. A zero divisor yields `lhs`, keeping `lhs == (lhs / rhs) * rhs + lhs % rhs` true for every input") \
	X(NEG,      "Integer arithmetic", 0, "num:i32 -- -num:i32", "Integer negation") \
	X(EQ,       "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs==rhs:u32", "Integer equality test") \
	X(NEQ,      "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs!=rhs:u32", "Integer inequality test") \
	X(SLT,      "Integer arithmetic", 0, "lhs:i32 rhs:i32 -- lhs<rhs:u32", "Signed integer less than") \
	X(SLE,      "Integer arithmetic", 0, "lhs:i32 rhs:i32 -- lhs<=rhs:u32", "Signed integer less than or equal") \
	X(ULT,      "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs<rhs:u32", "Unsigned integer less than") \
	X(ULE,      "Integer arithmetic", 0, "lhs:u32 rhs:u32 -- lhs<=rhs:u32", "Unsigned integer less than or equal") \
	\
	X(FADD,     "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs+rhs:f32", "Floating point addition") \
	X(FSUB,     "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs-rhs:f32", "Floating point subtraction") \
	X(FMUL,     "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs*rhs:f32", "Floating point multiplication") \
	X(FDIV,     "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs/rhs:f32", "Floating point division") \
	X(FMOD,     "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs%rhs:f32", "Floating point remainder") \
	X(FNEG,     "Floating point arithmetic", 0, "num:f32 -- -num:f32", "Floating point negation") \
	X(FEQ,      "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs==rhs:u32", "Floating point equality test") \
	X(FNEQ,     "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs!=rhs:u32", "Floating point inequality test") \
	X(FLT,      "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs<rhs:u32", "Floating point less than") \
	X(FLE,      "Floating point arithmetic", 0, "lhs:f32 rhs:f32 -- lhs<=rhs:u32", "Floating point less than or equal") \
	X(FNAN,     "Floating point arithmetic", 0, "value:f32 -- nan?:u32", "Check whether a value is NaN") \
	\
	X(AND,      "Bitwise manipulation", 0, "lhs:u32 rhs:u32 -- lhs&rhs:u32", "Bitwise and") \
	X(OR,       "Bitwise manipulation", 0, "lhs:u32 rhs:u32 -- lhs|rhs:u32", "Bitwise or") \
	X(XOR,      "Bitwise manipulation", 0, "lhs:u32 rhs:u32 -- lhs^rhs:u32", "Bitwise xor") \
	X(NOT,      "Bitwise manipulation", 0, "num:u32 -- ~num:u32", "Bitwise not") \
	X(SHL,      "Bitwise manipulation", 0, "num:u32 amount:u32 -- num<<amount:u32", "Bit-shift left. `amount` is masked to [0, 31] (`amount & 31`)") \
	X(SHR,      "Bitwise manipulation", 0, "num:u32 amount:u32 -- num>>amount:u32", "Logical shift right (no sign extend). `amount` is masked to [0, 31] (`amount & 31`)") \
	X(SAR,      "Bitwise manipulation", 0, "num:i32 amount:u32 -- num>>amount:i32", "Arithmetic shift right (sign extend). `amount` is masked to [0, 31] (`amount & 31`)") \
	\
	X(TMOVE,    "Temporary register", 0, "amount:i32 --", "Move the temporary register by the amount (vm.tp += amount)") \
	X(TSET,     "Temporary register", 0, "temp:u32 --", "Set the temporary register (vm.tp = temp)") \
	X(TGET,     "Temporary register", 0, "-- temp:u32", "Get the temporary register (temp = vm.tp)") \
	X(TMARK,    "Temporary register", 0, "value:u32 --", "Move the temporary mark **forward** to the given value (if (value > vm.tm) vm.tm = value)") \
	\
	X(ENTER,    "Aux frame", SHIBE_OPCODE_FLAG_IMM, "-- ; -- ..frame:aux-frame", "Allocate an auxiliary frame with N general purpose slots where N is the operand's value (vm.fp = vm.asp + header_size; vm.asp += header_size + imm; vm.tm = vm.tp)") \
	X(LEAVE,    "Aux frame", 0                    , "-- ; ..frame:aux-frame --", "Deallocate the auxiliary frame, restoring states, excluding the data stack") \
	X(UNWIND,   "Aux frame", 0                    , "..x -- ; ..frame:aux-frame --", "Deallocate the auxiliary frame, restoring states, including the data stack") \
	X(AGET,     "Aux frame", SHIBE_OPCODE_FLAG_IMM, "-- value:cell", "Retrieve a value from an auxiliary slot. The operand is a signed index: 0 and up reach the general purpose slots, negative values reach the frame header") \
	X(ASET,     "Aux frame", SHIBE_OPCODE_FLAG_IMM, "value:cell --", "Store a value into an auxiliary slot. The operand must be a non-negative index, the frame header is read only") \
	\
	X(EXTCALL,  "External call", SHIBE_OPCODE_FLAG_IMM | SHIBE_OPCODE_FLAG_ENDS_BUNDLE, "..a -- ..b", "Make a call to the host with the call number in the operand. Numbering starts at 1: call 0 is reserved and always faults as unbound, so an operand cell that was never patched cannot dispatch anywhere. The stack effect is unknown. The signature is usually predeclared and the host must take great care to not break the contract.") \

/*
 * # Auxiliary frame and temporary pointer
 *
 * These are the general purpose building block for many higher level features.
 *
 * ## Auxiliary frame
 *
 * `ENTER n` creates an auxiliary frame with the following structure:
 *
 * - saved_dsp: Set to the current value of `dsp` (data stack pointer)
 * - saved_fp: Set to the current value of `fp` (frame pointer)
 * - saved_tm: Set to the current value of `tm` (temporary mark)
 * - creator: Set to the address of this `ENTER`'s operand cell
 * - n slots: General purpose slot tail
 *
 * `fp` will now point at the tail of the frame (at general purpose slot 0).
 * `asp` is incremented by the size of this frame which is: n+4.
 * `tm` is then set to `tp` so the frame starts with an empty temporary arena.
 * `tm <= tp` therefore holds at all times.
 *
 * `creator` is never restored, it exists for stack walking. It names the code
 * that built the frame, and `n` can be read back from it with a single `FETCH`,
 * which is what lets a walker tell a frame's slots apart from the return
 * addresses `CALL` pushes above them. Without it the two are indistinguishable.
 *
 * `LEAVE` removes the current auxiliary frame:
 *
 * - `asp = fp - 4` (the header sits directly below `fp`)
 * - `fp = saved_fp`
 * - `tp = tm`
 * - `tm = saved_tm`
 *
 * Take note: `dsp` is **NOT** restored on purpose.
 *
 * `UNWIND` works the same way as `LEAVE` but with the following addition:
 *
 * - `dsp = saved_dsp`
 *
 * A stack walker has to test `creator` against 0 before it reads a frame.
 * Address 0 is reserved for special markers, and no real `ENTER` operand cell
 * can ever land there, so the value is unambiguous.
 *
 * This is a host frame, which has a different structure:
 *
 * - resume_ip: Where the run underneath carries on. **0** when there is no run
 *   underneath it
 * - continuation: The extcall number that finishes the call after a suspension.
 *   **0** for a call that cannot be suspended
 * - num_locals: How many locals follow
 * - saved_dsp, saved_fp, saved_tm: As in a frame `ENTER` built
 * - creator: Always **0**, which is what marks the frame as a host call's
 * - n locals: General purpose tail, as `ENTER`'s is
 *
 * `fp` points at the tail, so the header sits at `fp-1` .. `fp-4` exactly as it
 * does for a word's frame, and the three cells above are at `fp-5` (num_locals),
 * `fp-6` (continuation) and `fp-7` (resume_ip). `AGET` cannot reach past the
 * header, so a word can read a host frame's locals but never its control cells.
 *
 * A host call takes one with `shibe_alloc_frame`, and `shibe_execute` opens an
 * empty one for any re-entry that has not, so that the boundary is walkable.
 *
 * What a walker has to do differently:
 *
 * - The frame belongs to the host, not to any word, so there is no `ENTER` site
 *   to name it by. That is why `num_locals` is in the frame rather than read
 *   back from `creator`.
 * - Everything above it belongs to a nested run. Attributing those calls to the
 *   frame below the boundary would be wrong.
 * - The locals are whatever the host put there, so a walker should follow the
 *   host's convention. A call that stores its `__FILE__` and `__LINE__` is what
 *   lets a trace name a C location between two vm frames.
 * - `resume_ip` is what lets a trace carry on past the boundary. It is a bundle
 *   address, so a trace names a bundle rather than an opcode, the same
 *   granularity `creator` gives for a word's frame. For a call the vm made from
 *   an `EXTCALL` it is the bundle after the call.
 *   For one made from the debug hook it is the bundle the run had reached and
 *   has yet to run.
 *
 * `UNWIND` has to stop there for the same reason: unwinding past a live host
 * call would strand the host's own frame and return into a vm that had been
 * unwound out from under it.
 *
 * The data slots can be accessed with `AGET n` and `ASET n`.
 * `AGET` takes a signed index, so a frame can read its own header:
 *
 * - `AGET -1`: creator
 * - `AGET -2`: saved_tm
 * - `AGET -3`: saved_fp
 * - `AGET -4`: saved_dsp
 *
 * `ASET`, on the other hand, rejects a negative index.
 *
 * Using the auxiliary frames, several features can be built:
 *
 * - Local variables: Allocate a frame, store local variables inside general purpose slots.
 *   On `LEAVE`, the variables are removed.
 *   This can be used as temporary storage to help with stack shuffling and reduce shuffling noise.
 * - Exception handling: `UNWIND` until a "catch" frame is encountered, undoing all the stack effects.
 *   A frame type could, by convention, be stored in the first data slot (0).
 *   `UNWIND` restores the data stack to exactly what it was when `ENTER` ran, so
 *   anything pushed before `ENTER` survives it.
 *   A `[ risky ] [ handler ] catch` form pushes the handler, then `ENTER`s, and
 *   finds the handler back on top of the stack after the final `UNWIND`.
 *   The error record has to live in memory: it is produced inside the frame being
 *   unwound, so the data stack cannot carry it out.
 *   The outermost frame is expected to be a catch frame installed by the runtime.
 *   The VM does not check for one; unwinding past the last frame simply faults.
 *
 * ## Temporary pointer
 *
 * The VM provides a couple of pointers `tp` (temporary pointer) and `tm` (temporary mark).
 * `tp` can be freely modified.
 * `tp` is restored to `tm` on every `LEAVE` or `UNWIND`.
 *
 * On their own, the VM does not give any special meaning to these pointers.
 * However, they are usually used to build call-scoped arena:
 *
 * - With each allocation, bump `tp` and allocate memory in a region accordingly.
 * - Upon return from a function `tp` is reset to `tm`, making all temporarily allocated memories available for reuse.
 *
 * To return a variable sized structure to the caller, there are two options:
 *
 * - The caller allocates a large enough buffer for the callee to write over.
 * - The callee uses `TMARK` to move `tm` over an allocated structure.
 *   It will not be rolled over upon return.
 *   A high level language would expose something along the line of `retain var`
 *   where `var` points to an arena-allocated object.
 */

#define SHIBE_ENUM(NAME, GROUP, FLAGS, EFFECT, DESC) SHIBE_OP_ ## NAME,

SHIBE_FIXED_ENUM(shibe_opcode_t, uint8_t,
	SHIBE_OPCODE(SHIBE_ENUM)
);

_Static_assert(sizeof(shibe_opcode_t) == 1, "An opcode must fit in a byte to be bundled");

#define SHIBE_OPCODE_TO_STR(NAME, GROUP, FLAGS, EFFECT, DESC) case SHIBE_OP_ ## NAME: return #NAME;

static inline const char*
shibe_opcode_to_str(shibe_opcode_t opcode) {
	switch (opcode) {
		SHIBE_OPCODE(SHIBE_OPCODE_TO_STR)
		default: return NULL;
	}
}

#define SHIBE_OPCODE_TO_FLAGS(NAME, GROUP, FLAGS, EFFECT, DESC) case SHIBE_OP_ ## NAME: return (FLAGS);

static inline uint32_t
shibe_opcode_flags(shibe_opcode_t opcode) {
	switch (opcode) {
		SHIBE_OPCODE(SHIBE_OPCODE_TO_FLAGS)
		default: return SHIBE_OPCODE_FLAG_NONE;
	}
}

#define SHIBE_OPCODE_TO_GROUP(NAME, GROUP, FLAGS, EFFECT, DESC) case SHIBE_OP_ ## NAME: return GROUP;

static inline const char*
shibe_opcode_group(shibe_opcode_t opcode) {
	switch (opcode) {
		SHIBE_OPCODE(SHIBE_OPCODE_TO_GROUP)
		default: return NULL;
	}
}

#define SHIBE_OPCODE_TO_EFFECT(NAME, GROUP, FLAGS, EFFECT, DESC) case SHIBE_OP_ ## NAME: return EFFECT;

static inline const char*
shibe_opcode_effect(shibe_opcode_t opcode) {
	switch (opcode) {
		SHIBE_OPCODE(SHIBE_OPCODE_TO_EFFECT)
		default: return NULL;
	}
}

#define SHIBE_OPCODE_TO_DESC(NAME, GROUP, FLAGS, EFFECT, DESC) case SHIBE_OP_ ## NAME: return DESC;

static inline const char*
shibe_opcode_desc(shibe_opcode_t opcode) {
	switch (opcode) {
		SHIBE_OPCODE(SHIBE_OPCODE_TO_DESC)
		default: return NULL;
	}
}

typedef shibe_opcode_t shibe_bundle_t[4];

_Static_assert(sizeof(shibe_bundle_t) == sizeof(shibe_cell_t), "A bundle must be exactly one cell");

static inline shibe_cell_t
shibe_pack(const shibe_bundle_t bundle) {
	return (shibe_cell_t){
		.u32 =
			((uint32_t)bundle[0]      ) |
			((uint32_t)bundle[1] <<  8) |
			((uint32_t)bundle[2] << 16) |
			((uint32_t)bundle[3] << 24)
	};
}

static inline void
shibe_unpack(shibe_cell_t cell, shibe_bundle_t bundle) {
	bundle[0] = (uint8_t)(cell.u32      );
	bundle[1] = (uint8_t)(cell.u32 >>  8);
	bundle[2] = (uint8_t)(cell.u32 >> 16);
	bundle[3] = (uint8_t)(cell.u32 >> 24);
}

#endif
