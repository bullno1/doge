#ifndef SHIBE_OPCODE_H
#define SHIBE_OPCODE_H

#include <stddef.h>
#include <shibe.h>

#define SHIBE_OPCODE(X) \
	/* TRAP at 0, NOP is intentional */ \
	X(SHIBE_OP_TRAP) \
	X(SHIBE_OP_NOP) \
	/* Flow control */ \
	X(SHIBE_OP_JMP) \
	X(SHIBE_OP_JZ) \
	X(SHIBE_OP_CALL) \
	X(SHIBE_OP_CCALL) \
	X(SHIBE_OP_RET) \
	X(SHIBE_OP_HALT) \
	/* Stack manipulation */ \
	X(SHIBE_OP_LIT) \
	X(SHIBE_OP_DUP) \
	X(SHIBE_OP_DRP) \
	X(SHIBE_OP_SWP) \
	X(SHIBE_OP_ROT) \
	X(SHIBE_OP_NIP) \
	X(SHIBE_OP_OVR) \
	/* Memory */ \
	X(SHIBE_OP_FETCH) \
	X(SHIBE_OP_STORE) \
	X(SHIBE_OP_BFETCH) \
	X(SHIBE_OP_BSTORE) \
	X(SHIBE_OP_COPY) \
	/* Integer arithmetic */ \
	X(SHIBE_OP_ADD) \
	X(SHIBE_OP_SUB) \
	X(SHIBE_OP_MUL) \
	X(SHIBE_OP_SDIV) \
	X(SHIBE_OP_SREM) \
	X(SHIBE_OP_UDIV) \
	X(SHIBE_OP_UREM) \
	X(SHIBE_OP_NEG) \
	X(SHIBE_OP_EQ) \
	X(SHIBE_OP_NEQ) \
	X(SHIBE_OP_SLT) \
	X(SHIBE_OP_SLE) \
	X(SHIBE_OP_ULT) \
	X(SHIBE_OP_ULE) \
	/* Floating point arithmetic */ \
	X(SHIBE_OP_FADD) \
	X(SHIBE_OP_FSUB) \
	X(SHIBE_OP_FMUL) \
	X(SHIBE_OP_FDIV) \
	X(SHIBE_OP_FMOD) \
	X(SHIBE_OP_FNEG) \
	X(SHIBE_OP_FEQ) \
	X(SHIBE_OP_FNEQ) \
	X(SHIBE_OP_FLT) \
	X(SHIBE_OP_FLE) \
	X(SHIBE_OP_NAN) \
	/* Logic and Bit */ \
	X(SHIBE_OP_AND) \
	X(SHIBE_OP_OR) \
	X(SHIBE_OP_XOR) \
	X(SHIBE_OP_NOT) \
	X(SHIBE_OP_SHL) \
	X(SHIBE_OP_SHR) \
	X(SHIBE_OP_SAR) \
	/* Temp */ \
	X(SHIBE_OP_TMOVE) \
	X(SHIBE_OP_TSET) \
	X(SHIBE_OP_TGET) \
	X(SHIBE_OP_TMARK) \
	/* Aux frame */ \
	X(SHIBE_OP_ENTER) \
	X(SHIBE_OP_LEAVE) \
	X(SHIBE_OP_UNWIND) \
	X(SHIBE_OP_AGET) \
	X(SHIBE_OP_ASET) \
	/* External call */ \
	X(SHIBE_OP_EXTCALL) \

#define SHIBE_ENUM(OPCODE) OPCODE,

typedef enum : uint8_t {
	SHIBE_OPCODE(SHIBE_ENUM)
} shibe_opcode_t;

_Static_assert(sizeof(shibe_opcode_t) == 1, "An opcode must fit in a byte to be bundled");

#define SHIBE_OPCODE_TO_STR(OPCODE) case OPCODE: return &(#OPCODE[sizeof("SHIBE_OP_") - 1]);

static inline const char*
shibe_opcode_to_str(shibe_opcode_t opcode) {
	switch (opcode) {
		SHIBE_OPCODE(SHIBE_OPCODE_TO_STR)
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
