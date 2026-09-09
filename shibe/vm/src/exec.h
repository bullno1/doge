// No guard so this can be included multiple times
#include "internal.h"
#include <shibe/opcode.h>
#include <bmacro.h>

#define SHIBE_DISPATCH_METHOD_COMPUTED_GOTO 0
#define SHIBE_DISPATCH_METHOD_SWITCHED_GOTO 1
#define SHIBE_DISPATCH_METHOD_SWITCH        2

#define SHIBE_DISPATCH_METHOD SHIBE_DISPATCH_METHOD_SWITCH

#ifndef SHIBE_DISPATCH_METHOD
#	if defined(__GNUC__) || defined(__clang__)
#		define SHIBE_DISPATCH_METHOD SHIBE_DISPATCH_METHOD_COMPUTED_GOTO
#   else
#		define SHIBE_DISPATCH_METHOD SHIBE_DISPATCH_METHOD_SWITCHED_GOTO
#	endif
#endif

#if SHIBE_DISPATCH_METHOD == SHIBE_DISPATCH_METHOD_SWITCH

#define SHIBE_BEGIN_DISPATCH(opcode) switch (opcode) {

#define SHIBE_END_DISPATCH() }

#elif SHIBE_DISPATCH_METHOD == SHIBE_DISPATCH_METHOD_COMPUTED_GOTO

#define SHIBE_OPCODE_DISPATCH_TABLE_ENTRY(NAME) [SHIBE_OP_ ## VALUE] = &&op_ ## NAME,
#define SHIBE_BEGIN_DISPATCH() \
	static const void* dispatch_table[] = { \
		SHIBE_OPCODE(SHIBE_OPCODE_DISPATCH_TABLE_ENTRY) \
	}; \
	{ \
		SHIBE_NEXT_OPCODE();
#define SHIBE_END_DISPATCH() }
#define SHIBE_NEXT_OPCODE() \
	do { \
		SHIBE_DEBUG_HOOK(host, vm, &state, offset); \
		uint8_t opcode = mem[pc++]; \
		goto *dispatch_table[opcode]; \
	} while (0)

#elif SHIBE_DISPATCH_METHOD == SHIBE_DISPATCH_METHOD_SWITCHED_GOTO

#define SHIBE_BEGIN_DISPATCH() { SHIBE_NEXT_OPCODE();
#define SHIBE_END_DISPATCH() }
#define SHIBE_NEXT_OPCODE() \
	do { \
		SHIBE_DEBUG_HOOK() \
		uint8_t opcode = mem[pc++]; \
		switch (opcode) { \
			SHIBE_OPCODE(SHIBE_OPCODE_DISPATCH_TABLE_ENTRY) \
		} \
	} while (0)
#define SHIBE_OPCODE_DISPATCH_TABLE_ENTRY(NAME, VALUE) case VALUE: goto NAME;

#endif

// This is to make clangd stop complaining
#ifndef SHIBE_DEBUG_HOOK
#define SHIBE_DEBUG_HOOK()
#endif

#define SHIBE_LOAD_STATE(vm, state) do { state = vm->state; } while (0)
#define SHIBE_SAVE_STATE(vm, state) do { vm->state = state; } while (0)

static shibe_status_t
SHIBE_VM_EXECUTE(shibe_vm_t* vm) {
	shibe_host_t* host = vm->config.host;
	(void)host;
	return SHIBE_ERROR;
}
