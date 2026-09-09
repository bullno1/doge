// vim: set foldmethod=marker foldlevel=0:
// No guard so this can be included multiple times
//
// shibe.c defines SHIBE_EXEC_INLINED before including this. The interpreter is
// compiled as part of that file, so the real shibe_panic and the bseg
// implementation are already in the translation unit. Without it the header is
// being opened on its own and has to stand up by itself, so that an editor or a
// syntax check reports real problems instead of a wall of undefined symbols.
//
// Anything gated by SHIBE_EXEC_INLINED is a dummy definition to silence
// warnings or error caused by viewing this file as a standalone unit.
#ifndef SHIBE_EXEC_INLINED
#	define BSEG_IMPLEMENTATION
#	if defined(__GNUC__) || defined(__clang__)
#		pragma GCC diagnostic ignored "-Wunused-function"
#	endif
#endif

#include "internal.h"
#include <shibe/opcode.h>
#include <math.h>

#define SHIBE_DISPATCH_METHOD_COMPUTED_GOTO 0
#define SHIBE_DISPATCH_METHOD_SWITCHED_GOTO 1
#define SHIBE_DISPATCH_METHOD_SWITCH        2

#ifndef SHIBE_DISPATCH_METHOD
#	if defined(__GNUC__) || defined(__clang__)
#		define SHIBE_DISPATCH_METHOD SHIBE_DISPATCH_METHOD_COMPUTED_GOTO
#	else
#		define SHIBE_DISPATCH_METHOD SHIBE_DISPATCH_METHOD_SWITCH
#	endif
#endif

// Everything that does not depend on the caller's SHIBE_HAS_HOOK lives in here
// and is defined once, however many times this header is included. Only the
// region below the guard is rebuilt per variant.
// Common {{{
#ifndef SHIBE_EXEC_COMMON
#define SHIBE_EXEC_COMMON

// Only the computed goto path needs its warnings relaxed: the address of a
// label, `goto *` and the range designator that fills the table are all GNU
// extensions. The other two methods are standard C and keep full coverage, and
// a compiler that does not know `#pragma GCC diagnostic` never sees one.
#if SHIBE_DISPATCH_METHOD == SHIBE_DISPATCH_METHOD_COMPUTED_GOTO \
	&& (defined(__GNUC__) || defined(__clang__))
#	define SHIBE_RELAX_DIAGNOSTICS 1
#else
#	define SHIBE_RELAX_DIAGNOSTICS 0
#endif

// A pseudo opcode, never emitted, that sits above the four real ones in the
// decode window. Dispatching it is how the interpreter notices a bundle ran
// out, which keeps that test off the per opcode path entirely.
#define SHIBE_OP_ENDB 0xffu

#ifndef SHIBE_EXEC_INLINED
static void
shibe_panic(shibe_vm_t* vm, const shibe_panic_t* panic) { (void)vm; (void)panic; }

static bool
shibe_panicked(shibe_vm_t* vm) { (void)vm; return false; }
#endif

// Resolves an address to the cell it names, or NULL when it is out of bounds.
// The interpreter needs this rather than shibe_fetch/shibe_store so that it can
// write its registers back before the panic reaches the host.
static inline shibe_cell_t*
shibe_mem_ref(shibe_vm_t* vm, shibe_cell_t addr) {
	shibe_mem_seg_t* seg = &vm->regions[shibe_mem_region(addr)];
	uint32_t index = shibe_mem_index(addr);
	return index < bseg_len(*seg) ? bseg_ref(*seg, index) : NULL;
}

// A run of cells that plain indexing can reach, so that instruction fetch does
// not pay for a full address resolution on every bundle and every immediate.
//
// `addr` carries the region bits, so an address in another region falls out of
// range on its own and no region check is needed on the fast path.
typedef struct {
	uint32_t addr;
	uint32_t len;
	shibe_cell_t* base;
} shibe_span_t;

// Locate the span holding `addr`: its bseg segment, clipped to the region's
// allocated length. A segment is separately allocated, so a span never crosses
// one, and clipping to the length folds the bounds check into the range test.
//
// The result stays good for the length of a run. Segments are stable once
// allocated and growth only appends, so a cached span can go stale only by
// being shorter than the region now is, which costs a re-locate and never a
// wrong answer.
//
// Returns false when `addr` is out of bounds, leaving `span` untouched.
static inline bool
shibe_span_locate(shibe_vm_t* vm, shibe_cell_t addr, shibe_span_t* span) {
	shibe_mem_seg_t* seg = &vm->regions[shibe_mem_region(addr)];
	uint32_t index = shibe_mem_index(addr);
	size_t len = bseg_len(*seg);
	if (index >= len) { return false; }

	int segment = bseg__segment_of(index);
	size_t base = bseg__capacity_for(segment);
	size_t end = base + bseg__segment_len(segment);
	if (end > len) { end = len; }

	span->addr = addr.u32 - index + (uint32_t)base;
	span->len = (uint32_t)(end - base);
	span->base = bseg_ref(*seg, base);
	return true;
}

// Bulk cell copy with memmove semantics. Both ranges are checked before
// anything is written, so a copy that faults leaves memory untouched. Staying
// within bseg_len also keeps it inside one region.
static inline bool
shibe_mem_copy(shibe_vm_t* vm, shibe_cell_t dst, shibe_cell_t src, uint32_t len) {
	if (len == 0) { return true; }

	shibe_mem_seg_t* dst_seg = &vm->regions[shibe_mem_region(dst)];
	shibe_mem_seg_t* src_seg = &vm->regions[shibe_mem_region(src)];
	uint32_t dst_index = shibe_mem_index(dst);
	uint32_t src_index = shibe_mem_index(src);
	size_t dst_len = bseg_len(*dst_seg);
	size_t src_len = bseg_len(*src_seg);

	if (dst_index >= dst_len || dst_len - dst_index < len) { return false; }
	if (src_index >= src_len || src_len - src_index < len) { return false; }

	if (dst_seg == src_seg && dst_index > src_index) {
		for (uint32_t i = len; i-- > 0;) {
			*bseg_ref(*dst_seg, dst_index + i) = *bseg_ref(*src_seg, src_index + i);
		}
	} else {
		for (uint32_t i = 0; i < len; ++i) {
			*bseg_ref(*dst_seg, dst_index + i) = *bseg_ref(*src_seg, src_index + i);
		}
	}

	return true;
}

// INT32_MIN / -1 is the one signed division that overflows; wrap to the two's
// complement result rather than letting the host trap
static inline int32_t
shibe_sdiv(int32_t lhs, int32_t rhs) {
	return (lhs == INT32_MIN && rhs == -1) ? INT32_MIN : lhs / rhs;
}

static inline int32_t
shibe_srem(int32_t lhs, int32_t rhs) {
	return (lhs == INT32_MIN && rhs == -1) ? 0 : lhs % rhs;
}

#define SHIBE_LOAD_STATE(vm, state) do { (state) = (vm)->state; } while (0)
// exec_state belongs to the vm, not to the register file: a panic raised from
// under us has already written it and must not be rolled back
#define SHIBE_SAVE_STATE(vm, state) \
	do { \
		shibe_exec_state_t exec_state_ = (vm)->state.exec_state; \
		(vm)->state = (state); \
		(vm)->state.exec_state = exec_state_; \
	} while (0)

#define SHIBE_FAULT(ERROR, ARG) \
	do { \
		SHIBE_SAVE_STATE(vm, state); \
		shibe_panic(vm, &(shibe_panic_t){ .error = (ERROR), .arg = (ARG) }); \
		return SHIBE_ERROR; \
	} while (0)

// Calls a host callback and makes what it returned mean something.
//
// The callback runs with the vm's registers written back, so it can reach in
// through the public api, and with an activation of its own: `hfp` starts empty,
// so a frame it opens is its own rather than its caller's. Anything it left on
// the auxiliary stack belongs to that call, so a callback that returns normally
// has it taken back here. A suspension or a panic keeps it: the frame is what a
// resume finishes the call through, and what a stack walker reads afterwards.
//
// The callback may have failed on its own, or it may be relaying something that
// was already raised on this vm from underneath it: by a nested shibe_execute,
// or by any public api call that faulted. Only the first of those is new. A
// re-raised panic would fire the host's panic handler a second time and lose the
// original reason, and a re-recorded suspension would overwrite what the inner
// run left. `exec_state` tells the two apart, and it wins over what the callback
// returned either way: one that reports SHIBE_OK on top of a vm that has already
// panicked or suspended is still a stop.
//
// `RESUME_IP` is where the run underneath carries on if this call stops, which
// is what a frame it opens records and what a suspension is written down
// against. It is not what the callback is shown: `ip` stays the cursor it really
// is, so a debug hook inspects the vm as it stands rather than a value arranged
// for it. `CAN_SUSPEND` is whether that address describes where the run actually
// is - a callback that stops where it does not panics instead.
// What has to be put back when the callback returns. It rides the C stack
// across the call rather than the register file, so the interpreter's `state`
// never has its address taken and stays in registers.
typedef struct {
	shibe_cell_t saved_ip;
	shibe_cell_t saved_fp;
	shibe_cell_t saved_resume_ip;
	shibe_cell_t resume_ip;
	uint32_t saved_asp;
	uint32_t saved_hfp;
	bool saved_can_suspend;
	bool can_suspend;
} shibe_host_call_t;

static inline shibe_host_call_t
shibe_host_call_begin(shibe_vm_t* vm, shibe_cell_t resume_ip, bool can_suspend) {
	shibe_host_call_t call = {
		.saved_ip = vm->state.ip,
		.saved_fp = vm->state.fp,
		.saved_asp = vm->state.asp.u32,
		.saved_hfp = vm->hfp,
		.saved_can_suspend = vm->can_suspend,
		.saved_resume_ip = vm->resume_ip,
		.resume_ip = resume_ip,
		.can_suspend = can_suspend,
	};
	vm->hfp = 0;
	vm->resume_ip = resume_ip;
	vm->can_suspend = can_suspend;
	return call;
}

// Everything a host call has to sort out once the callback is back. It works on
// `vm->state`, which the caller wrote the register file into, so nothing here
// needs to reach into the interpreter's locals.
//
// Returns SHIBE_OK when the run carries on, otherwise the status the run has to
// end with. The interpreter never returns SHIBE_OK from a host call site, so
// there is no ambiguity in reusing it to mean "carry on".
static shibe_status_t
shibe_host_call_end(
	shibe_vm_t* vm,
	const shibe_host_call_t* call,
	shibe_status_t status,
	shibe_error_t error,
	shibe_cell_t arg,
	shibe_cell_t resume_ip
) {
	uint32_t host_fp = vm->hfp;
	vm->hfp = call->saved_hfp;
	vm->resume_ip = call->saved_resume_ip;
	vm->can_suspend = call->saved_can_suspend;

	bool relayed = vm->state.exec_state == SHIBE_EXEC_SUSPENDED;
	if (relayed && status == SHIBE_OK) { status = SHIBE_SUSPENDED; }

	if (status == SHIBE_OK && !shibe_panicked(vm)) {
		vm->state.ip = call->saved_ip;
		vm->state.asp.u32 = call->saved_asp;
		vm->state.fp = call->saved_fp;
	}

	if (shibe_panicked(vm)) { return SHIBE_ERROR; }

	if (status == SHIBE_ERROR) {
		shibe_panic(vm, &(shibe_panic_t){ .error = error, .arg = arg });
		return SHIBE_ERROR;
	}

	if (status == SHIBE_SUSPENDED) {
		if (!relayed) {
			if (!call->can_suspend) {
				shibe_panic(vm, &(shibe_panic_t){
					.error = SHIBE_ERR_NOT_SUSPENDABLE,
				});
				return SHIBE_ERROR;
			}
			if (host_fp != 0) {
				// A frame nobody can finish would strand both itself and
				// whatever ran under it
				if (vm->state.as[host_fp + SHIBE_AUX_HOST_CONTINUATION].u32 == 0) {
					shibe_panic(vm, &(shibe_panic_t){
						.error = SHIBE_ERR_NOT_SUSPENDABLE,
						.arg = (shibe_cell_t){ .u32 = host_fp },
					});
					return SHIBE_ERROR;
				}
				vm->at_continuation = true;
			}
		}
		// Only now does `ip` become the resume point: while the callback was
		// running it was the cursor, which is what it really was.
		//
		// `resume_ip` is read again here rather than reused from `begin`: for an
		// EXTCALL it is `ip` itself, and a nested run that suspended underneath
		// has already moved it to where that run stopped. Coming back to the
		// outer call site instead would lose the inner activation.
		vm->state.ip = resume_ip;
		vm->state.exec_state = SHIBE_EXEC_SUSPENDED;
		return SHIBE_SUSPENDED;
	}

	return SHIBE_OK;
}

#define SHIBE_HOST_CALL(CALL, ERROR, ARG, RESUME_IP, CAN_SUSPEND) \
	do { \
		SHIBE_SAVE_STATE(vm, state); \
		shibe_host_call_t call_ = \
			shibe_host_call_begin(vm, (RESUME_IP), (CAN_SUSPEND)); \
		shibe_status_t host_status_ = (CALL); \
		host_status_ = shibe_host_call_end( \
			vm, &call_, host_status_, (ERROR), (ARG), (RESUME_IP) \
		); \
		if (host_status_ != SHIBE_OK) { return host_status_; } \
		SHIBE_LOAD_STATE(vm, state); \
	} while (0)

// Reads the cell `ip` names and steps over it.
//
// The span cache carries the common case: a subtract, an unsigned compare and
// an indexed load. Only a fetch that leaves the cached span pays for the
// segment lookup, and because the test is on the address rather than on a
// walking pointer, a jump whose target is still in the span stays on the fast
// path too.
#define SHIBE_FETCH_IP(OUT) \
	do { \
		uint32_t off_ = state.ip.u32 - span.addr; \
		if (off_ >= span.len) { \
			if (!shibe_span_locate(vm, state.ip, &span)) { \
				SHIBE_FAULT(SHIBE_ERR_MEM_FAULT, state.ip); \
			} \
			off_ = state.ip.u32 - span.addr; \
		} \
		state.ip.u32 += 1; \
		(OUT) = span.base[off_]; \
	} while (0)

// Reads the operand cell the instruction stream is sitting on
#define SHIBE_IMM(OUT) SHIBE_FETCH_IP(OUT)

// Depth is checked once per handler, then the slots are addressed directly.
// SHIBE_DS(0) is the top of the stack.
#define SHIBE_DS_NEED(N) \
	do { \
		if (state.dsp.u32 < (uint32_t)(N)) { \
			SHIBE_FAULT(SHIBE_ERR_STACK_UNDERFLOW, SHIBE_STACK_DS); \
		} \
	} while (0)
#define SHIBE_DS_ROOM(N) \
	do { \
		if (ds_len - state.dsp.u32 < (uint32_t)(N)) { \
			SHIBE_FAULT(SHIBE_ERR_STACK_OVERFLOW, SHIBE_STACK_DS); \
		} \
	} while (0)
#define SHIBE_DS(I)       (state.ds[state.dsp.u32 - 1 - (uint32_t)(I)])
#define SHIBE_DS_DROP(N)  do { state.dsp.u32 -= (uint32_t)(N); } while (0)
#define SHIBE_DS_PUSH(V) \
	do { \
		shibe_cell_t pushed_ = (V); \
		state.ds[state.dsp.u32] = pushed_; \
		state.dsp.u32 += 1; \
	} while (0)

#define SHIBE_AS_NEED(N) \
	do { \
		if (state.asp.u32 < (uint32_t)(N)) { \
			SHIBE_FAULT(SHIBE_ERR_STACK_UNDERFLOW, SHIBE_STACK_AS); \
		} \
	} while (0)
#define SHIBE_AS_ROOM(N) \
	do { \
		if (as_len - state.asp.u32 < (uint32_t)(N)) { \
			SHIBE_FAULT(SHIBE_ERR_STACK_OVERFLOW, SHIBE_STACK_AS); \
		} \
	} while (0)
#define SHIBE_AS_PUSH(V) \
	do { \
		shibe_cell_t pushed_ = (V); \
		state.as[state.asp.u32] = pushed_; \
		state.asp.u32 += 1; \
	} while (0)
#define SHIBE_AS_POP()    (state.as[--state.asp.u32])

#define SHIBE_REFILL() \
	do { \
		shibe_cell_t bundle_; \
		SHIBE_TRACE_REFILL(); \
		SHIBE_FETCH_IP(bundle_); \
		win = (uint64_t)bundle_.u32 | ((uint64_t)SHIBE_OP_ENDB << 32); \
	} while (0)

// Dispatch methods {{{
#if SHIBE_DISPATCH_METHOD == SHIBE_DISPATCH_METHOD_SWITCH
// Switch {{{

#define SHIBE_BEGIN_DISPATCH() \
	for (;;) { \
		uint8_t opcode = (uint8_t)win; win >>= 8; \
		SHIBE_TRACE_STEP(); \
		switch (opcode) { \
			case SHIBE_OP_ENDB: SHIBE_NEXT_BUNDLE();
#define SHIBE_OP(NAME)          case SHIBE_OP_ ## NAME:
#define SHIBE_OP_DEFAULT(NAME)  default: case SHIBE_OP_ ## NAME:
#define SHIBE_END_OP()          break;
// `break` has to escape the switch, so this one cannot be wrapped in do/while
#define SHIBE_NEXT_BUNDLE()     { SHIBE_REFILL(); break; }
#define SHIBE_END_DISPATCH()    } }

// }}}
#elif SHIBE_DISPATCH_METHOD == SHIBE_DISPATCH_METHOD_COMPUTED_GOTO
// Computed goto {{{
#define SHIBE_DISPATCH_ENTRY(NAME, GROUP, FLAGS, EFFECT, DESC) \
	[SHIBE_OP_ ## NAME] = &&shibe_op_ ## NAME,
#define SHIBE_BEGIN_DISPATCH() \
	static const void* const dispatch[256] = { \
		[0 ... 255] = &&shibe_op_TRAP, \
		SHIBE_OPCODE(SHIBE_DISPATCH_ENTRY) \
		[SHIBE_OP_ENDB] = &&shibe_op_ENDB, \
	}; \
	SHIBE_NEXT(); \
	shibe_op_ENDB: SHIBE_NEXT_BUNDLE();
#define SHIBE_OP(NAME)          shibe_op_ ## NAME:
#define SHIBE_OP_DEFAULT(NAME)  shibe_op_ ## NAME:
#define SHIBE_END_OP()          SHIBE_NEXT();
#define SHIBE_NEXT() \
	do { \
		uint8_t opcode = (uint8_t)win; win >>= 8; \
		SHIBE_TRACE_STEP(); \
		goto *dispatch[opcode]; \
	} while (0)
#define SHIBE_NEXT_BUNDLE()     do { SHIBE_REFILL(); SHIBE_NEXT(); } while (0)
#define SHIBE_END_DISPATCH()
// }}}
#elif SHIBE_DISPATCH_METHOD == SHIBE_DISPATCH_METHOD_SWITCHED_GOTO
// Switched goto {{{
#define SHIBE_DISPATCH_ENTRY(NAME, GROUP, FLAGS, EFFECT, DESC) \
	case SHIBE_OP_ ## NAME: goto shibe_op_ ## NAME;
#define SHIBE_BEGIN_DISPATCH() \
	SHIBE_NEXT(); \
	shibe_op_ENDB: SHIBE_NEXT_BUNDLE();
#define SHIBE_OP(NAME)          shibe_op_ ## NAME:
#define SHIBE_OP_DEFAULT(NAME)  shibe_op_ ## NAME:
#define SHIBE_END_OP()          SHIBE_NEXT();
#define SHIBE_NEXT() \
	do { \
		uint8_t opcode = (uint8_t)win; win >>= 8; \
		SHIBE_TRACE_STEP(); \
		switch (opcode) { \
			SHIBE_OPCODE(SHIBE_DISPATCH_ENTRY) \
			case SHIBE_OP_ENDB: goto shibe_op_ENDB; \
			default: goto shibe_op_TRAP; \
		} \
	} while (0)
#define SHIBE_NEXT_BUNDLE()     do { SHIBE_REFILL(); SHIBE_NEXT(); } while (0)
#define SHIBE_END_DISPATCH()
// }}}
#endif
// }}}

#define SHIBE_U32(V)  ((shibe_cell_t){ .u32 = (uint32_t)(V) })
#define SHIBE_I32(V)  ((shibe_cell_t){ .i32 = (int32_t)(V) })
#define SHIBE_F32(V)  ((shibe_cell_t){ .f32 = (float)(V) })
#define SHIBE_BOOL(V) SHIBE_U32((V) ? 1u : 0u)

// `lhs` is the cell under the top, `rhs` the top; the result replaces both
#define SHIBE_BIN_OP(RESULT) \
	do { \
		SHIBE_DS_NEED(2); \
		shibe_cell_t lhs = SHIBE_DS(1); \
		shibe_cell_t rhs = SHIBE_DS(0); \
		SHIBE_DS_DROP(1); \
		SHIBE_DS(0) = (RESULT); \
	} while (0)

#define SHIBE_UN_OP(RESULT) \
	do { \
		SHIBE_DS_NEED(1); \
		shibe_cell_t num = SHIBE_DS(0); \
		SHIBE_DS(0) = (RESULT); \
	} while (0)

// Keeps arithmetic total: a zero divisor produces a quotient of zero rather
// than faulting
#define SHIBE_DIV_OP(RESULT) \
	do { \
		SHIBE_DS_NEED(2); \
		if (SHIBE_DS(0).u32 == 0) { \
			SHIBE_DS_DROP(1); \
			SHIBE_DS(0) = SHIBE_ZERO; \
		} else { \
			SHIBE_BIN_OP(RESULT); \
		} \
	} while (0)

// The remainder of a zero divisor is the dividend, which is what keeps
// `lhs == (lhs / rhs) * rhs + lhs % rhs` true for every pair of inputs. Dropping
// the divisor already leaves the dividend on top, so there is nothing to write.
#define SHIBE_REM_OP(RESULT) \
	do { \
		SHIBE_DS_NEED(2); \
		if (SHIBE_DS(0).u32 == 0) { \
			SHIBE_DS_DROP(1); \
		} else { \
			SHIBE_BIN_OP(RESULT); \
		} \
	} while (0)

#define SHIBE_LEAVE_FRAME(RESTORE_DS) \
	do { \
		if (state.fp.u32 < SHIBE_AUX_HEADER_LEN || state.fp.u32 > state.asp.u32) { \
			SHIBE_FAULT(SHIBE_ERR_STACK_UNDERFLOW, SHIBE_STACK_AS); \
		} \
		uint32_t base_ = state.fp.u32 - SHIBE_AUX_HEADER_LEN; \
		shibe_cell_t saved_dsp_ = state.as[base_ + 0]; \
		shibe_cell_t saved_fp_ = state.as[base_ + 1]; \
		shibe_cell_t saved_tm_ = state.as[base_ + 2]; \
		state.asp.u32 = base_; \
		state.fp = saved_fp_; \
		state.tp = state.tm; \
		state.tm = saved_tm_; \
		if (RESTORE_DS) { state.dsp = saved_dsp_; } \
	} while (0)

#endif
// }}}

// Keeps clangd quiet when this header is opened on its own
#ifndef SHIBE_HAS_HOOK
#define SHIBE_HAS_HOOK 0
#endif
#ifndef SHIBE_VM_EXECUTE
#define SHIBE_VM_EXECUTE shibe_execute_standalone
#endif

// The one region that varies: the tracing hooks fold away entirely in the build
// without a debug hook, so they are redefined for each variant.
#undef SHIBE_TRACE_DECL
#undef SHIBE_TRACE_REFILL
#undef SHIBE_TRACE_STEP

#if SHIBE_HAS_HOOK
#	define SHIBE_TRACE_DECL()   shibe_op_addr_t at = { 0 };
#	define SHIBE_TRACE_REFILL() do { at.bundle = state.ip; at.slot = 0xff; } while (0)
// The window bootstraps on a synthetic SHIBE_OP_ENDB, and every bundle ends on
// one. Neither is an instruction, so neither is reported.
//
// `ip` is a cursor: a refill moves it past the bundle cell and every immediate
// moves it past an operand, so by the last slot it is already into the next
// bundle. The hook sees that, because that is the truth of where the vm is; the
// bundle it is reporting is handed to it separately, and given to the vm as the
// point the run would carry on from.
//
// Slot 0 is the only place a hook can stop, because it is the only slot that
// address describes: nothing in the bundle has run there and no operand has
// been consumed, so coming back to it re-does nothing. Anywhere else the same
// address would re-run the slots before it, which is why stopping there is
// refused rather than written down wrong.
//
// So the bundle is only load bearing at slot 0. A frame the hook opens further
// in records it too, but nothing can ever come back through that frame - every
// route to a suspension crossing it is refused by `can_suspend` - and it is
// read there only by a stack walker, which wants the bundle a run is inside
// rather than the cell its cursor happens to be resting on.
#	define SHIBE_TRACE_STEP() \
		do { \
			if (opcode != SHIBE_OP_ENDB) { \
				at.slot += 1; \
				SHIBE_HOST_CALL( \
					host->debug(host, vm, &vm->state, at), \
					SHIBE_ERR_HOOK, SHIBE_ZERO, at.bundle, at.slot == 0 \
				); \
			} \
		} while (0)
#else
#	define SHIBE_TRACE_DECL()
#	define SHIBE_TRACE_REFILL()
#	define SHIBE_TRACE_STEP()
#endif

#if SHIBE_RELAX_DIAGNOSTICS
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wpedantic"
#	ifdef __clang__
#		pragma GCC diagnostic ignored "-Wgnu-label-as-value"
#		pragma GCC diagnostic ignored "-Winitializer-overrides"
#	else
#		pragma GCC diagnostic ignored "-Woverride-init"
#	endif
#endif

static shibe_status_t
SHIBE_VM_EXECUTE(shibe_vm_t* vm) {
	shibe_host_t* host = vm->config.host;
	uint32_t ds_len = vm->config.ds_len;
	uint32_t as_len = vm->config.as_len;

	// The register file lives in locals for the length of the run
	shibe_state_t state;
	SHIBE_LOAD_STATE(vm, state);

	// The opcodes of the current bundle, lowest slot first, with the pseudo
	// opcode above them. Every dispatch shifts one byte out. A run bootstraps on
	// the pseudo opcode alone, so its first dispatch refills from `ip` - which is
	// also all a resume has to do, since nothing may stop anywhere but a bundle
	// boundary.
	uint64_t win = SHIBE_OP_ENDB;

	// Where instruction fetch is reading from. A zero span has no cells in it,
	// so the first fetch of the run always goes and locates one. A resume gets a
	// fresh one for the same reason: it re-enters here with the span empty.
	shibe_span_t span = { 0 };

	SHIBE_TRACE_DECL();

	SHIBE_BEGIN_DISPATCH()

	SHIBE_OP(NOP) {
	} SHIBE_END_OP()

	/* Flow control */

	SHIBE_OP(JMP) {
		shibe_cell_t target;
		SHIBE_IMM(target);
		state.ip = target;
		SHIBE_NEXT_BUNDLE();
	}

	SHIBE_OP(JZ) {
		shibe_cell_t target;
		SHIBE_IMM(target);
		SHIBE_DS_NEED(1);
		shibe_cell_t cond = SHIBE_DS(0);
		SHIBE_DS_DROP(1);
		if (cond.u32 == 0) { state.ip = target; }
		SHIBE_NEXT_BUNDLE();
	}

	SHIBE_OP(CALL) {
		SHIBE_DS_NEED(1);
		SHIBE_AS_ROOM(1);
		shibe_cell_t target = SHIBE_DS(0);
		SHIBE_DS_DROP(1);
		// Nothing follows a control transfer in a bundle, so `ip` is already
		// the address of the next instruction
		SHIBE_AS_PUSH(state.ip);
		state.ip = target;
		SHIBE_NEXT_BUNDLE();
	}

	SHIBE_OP(CCALL) {
		SHIBE_DS_NEED(2);
		shibe_cell_t target = SHIBE_DS(0);
		shibe_cell_t cond = SHIBE_DS(1);
		SHIBE_DS_DROP(2);
		if (cond.u32 != 0) {
			SHIBE_AS_ROOM(1);
			SHIBE_AS_PUSH(state.ip);
			state.ip = target;
		}
		SHIBE_NEXT_BUNDLE();
	}

	SHIBE_OP(RET) {
		SHIBE_AS_NEED(1);
		state.ip = SHIBE_AS_POP();
		SHIBE_NEXT_BUNDLE();
	}

	SHIBE_OP(HALT) {
		SHIBE_SAVE_STATE(vm, state);
		vm->state.exec_state = SHIBE_EXEC_IDLE;
		return SHIBE_OK;
	}

	/* Stack manipulation */

	SHIBE_OP(LIT) {
		shibe_cell_t operand;
		SHIBE_IMM(operand);
		SHIBE_DS_ROOM(1);
		SHIBE_DS_PUSH(operand);
	} SHIBE_END_OP()

	SHIBE_OP(DUP) {
		SHIBE_DS_NEED(1);
		SHIBE_DS_ROOM(1);
		SHIBE_DS_PUSH(SHIBE_DS(0));
	} SHIBE_END_OP()

	SHIBE_OP(DRP) {
		SHIBE_DS_NEED(1);
		SHIBE_DS_DROP(1);
	} SHIBE_END_OP()

	SHIBE_OP(SWP) {
		SHIBE_DS_NEED(2);
		shibe_cell_t top = SHIBE_DS(0);
		SHIBE_DS(0) = SHIBE_DS(1);
		SHIBE_DS(1) = top;
	} SHIBE_END_OP()

	SHIBE_OP(ROT) {
		SHIBE_DS_NEED(3);
		shibe_cell_t a = SHIBE_DS(2);
		SHIBE_DS(2) = SHIBE_DS(1);
		SHIBE_DS(1) = SHIBE_DS(0);
		SHIBE_DS(0) = a;
	} SHIBE_END_OP()

	SHIBE_OP(NIP) {
		SHIBE_DS_NEED(2);
		SHIBE_DS(1) = SHIBE_DS(0);
		SHIBE_DS_DROP(1);
	} SHIBE_END_OP()

	SHIBE_OP(OVR) {
		SHIBE_DS_NEED(2);
		SHIBE_DS_ROOM(1);
		SHIBE_DS_PUSH(SHIBE_DS(1));
	} SHIBE_END_OP()

	/* Memory */

	SHIBE_OP(FETCH) {
		SHIBE_DS_NEED(1);
		shibe_cell_t addr = SHIBE_DS(0);
		shibe_cell_t* ref = shibe_mem_ref(vm, addr);
		if (ref == NULL) { SHIBE_FAULT(SHIBE_ERR_MEM_FAULT, addr); }
		SHIBE_DS(0) = *ref;
	} SHIBE_END_OP()

	SHIBE_OP(STORE) {
		SHIBE_DS_NEED(2);
		shibe_cell_t addr = SHIBE_DS(0);
		shibe_cell_t value = SHIBE_DS(1);
		shibe_cell_t* ref = shibe_mem_ref(vm, addr);
		if (ref == NULL) { SHIBE_FAULT(SHIBE_ERR_MEM_FAULT, addr); }
		SHIBE_DS_DROP(2);
		*ref = value;
	} SHIBE_END_OP()

	SHIBE_OP(BFETCH) {
		SHIBE_DS_NEED(2);
		shibe_cell_t addr = SHIBE_DS(1);
		uint32_t shift = (SHIBE_DS(0).u32 & 3u) * 8u;
		shibe_cell_t* ref = shibe_mem_ref(vm, addr);
		if (ref == NULL) { SHIBE_FAULT(SHIBE_ERR_MEM_FAULT, addr); }
		SHIBE_DS_DROP(1);
		SHIBE_DS(0) = SHIBE_U32((ref->u32 >> shift) & 0xffu);
	} SHIBE_END_OP()

	SHIBE_OP(BSTORE) {
		SHIBE_DS_NEED(3);
		shibe_cell_t addr = SHIBE_DS(1);
		uint32_t shift = (SHIBE_DS(0).u32 & 3u) * 8u;
		uint32_t value = SHIBE_DS(2).u32 & 0xffu;
		shibe_cell_t* ref = shibe_mem_ref(vm, addr);
		if (ref == NULL) { SHIBE_FAULT(SHIBE_ERR_MEM_FAULT, addr); }
		SHIBE_DS_DROP(3);
		ref->u32 = (ref->u32 & ~(0xffu << shift)) | (value << shift);
	} SHIBE_END_OP()

	SHIBE_OP(COPY) {
		SHIBE_DS_NEED(3);
		shibe_cell_t dst = SHIBE_DS(2);
		shibe_cell_t src = SHIBE_DS(1);
		uint32_t len = SHIBE_DS(0).u32;
		if (!shibe_mem_copy(vm, dst, src, len)) {
			SHIBE_FAULT(SHIBE_ERR_MEM_FAULT, dst);
		}
		SHIBE_DS_DROP(3);
	} SHIBE_END_OP()

	/* Integer arithmetic */

	SHIBE_OP(ADD)  { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 + rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(SUB)  { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 - rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(MUL)  { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 * rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(SDIV) { SHIBE_DIV_OP(SHIBE_I32(shibe_sdiv(lhs.i32, rhs.i32))); } SHIBE_END_OP()
	SHIBE_OP(SREM) { SHIBE_REM_OP(SHIBE_I32(shibe_srem(lhs.i32, rhs.i32))); } SHIBE_END_OP()
	SHIBE_OP(UDIV) { SHIBE_DIV_OP(SHIBE_U32(lhs.u32 / rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(UREM) { SHIBE_REM_OP(SHIBE_U32(lhs.u32 % rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(NEG)  { SHIBE_UN_OP(SHIBE_U32(0u - num.u32)); } SHIBE_END_OP()
	SHIBE_OP(EQ)   { SHIBE_BIN_OP(SHIBE_BOOL(lhs.u32 == rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(NEQ)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.u32 != rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(SLT)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.i32 <  rhs.i32)); } SHIBE_END_OP()
	SHIBE_OP(SLE)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.i32 <= rhs.i32)); } SHIBE_END_OP()
	SHIBE_OP(ULT)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.u32 <  rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(ULE)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.u32 <= rhs.u32)); } SHIBE_END_OP()

	/* Floating point arithmetic */

	SHIBE_OP(FADD) { SHIBE_BIN_OP(SHIBE_F32(lhs.f32 + rhs.f32)); } SHIBE_END_OP()
	SHIBE_OP(FSUB) { SHIBE_BIN_OP(SHIBE_F32(lhs.f32 - rhs.f32)); } SHIBE_END_OP()
	SHIBE_OP(FMUL) { SHIBE_BIN_OP(SHIBE_F32(lhs.f32 * rhs.f32)); } SHIBE_END_OP()
	SHIBE_OP(FDIV) { SHIBE_BIN_OP(SHIBE_F32(lhs.f32 / rhs.f32)); } SHIBE_END_OP()
	SHIBE_OP(FMOD) { SHIBE_BIN_OP(SHIBE_F32(fmodf(lhs.f32, rhs.f32))); } SHIBE_END_OP()
	SHIBE_OP(FNEG) { SHIBE_UN_OP(SHIBE_F32(-num.f32)); } SHIBE_END_OP()
	SHIBE_OP(FEQ)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.f32 == rhs.f32)); } SHIBE_END_OP()
	SHIBE_OP(FNEQ) { SHIBE_BIN_OP(SHIBE_BOOL(lhs.f32 != rhs.f32)); } SHIBE_END_OP()
	SHIBE_OP(FLT)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.f32 <  rhs.f32)); } SHIBE_END_OP()
	SHIBE_OP(FLE)  { SHIBE_BIN_OP(SHIBE_BOOL(lhs.f32 <= rhs.f32)); } SHIBE_END_OP()
	// Only a NaN compares unequal to itself, so this needs no math.h
	SHIBE_OP(FNAN) { SHIBE_UN_OP(SHIBE_BOOL(num.f32 != num.f32)); } SHIBE_END_OP()

	/* Bitwise manipulation */

	SHIBE_OP(AND) { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 & rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(OR)  { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 | rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(XOR) { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 ^ rhs.u32)); } SHIBE_END_OP()
	SHIBE_OP(NOT) { SHIBE_UN_OP(SHIBE_U32(~num.u32)); } SHIBE_END_OP()
	// A shift wider than the cell is masked rather than left undefined
	SHIBE_OP(SHL) { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 << (rhs.u32 & 31u))); } SHIBE_END_OP()
	SHIBE_OP(SHR) { SHIBE_BIN_OP(SHIBE_U32(lhs.u32 >> (rhs.u32 & 31u))); } SHIBE_END_OP()
	SHIBE_OP(SAR) { SHIBE_BIN_OP(SHIBE_I32(lhs.i32 >> (rhs.u32 & 31u))); } SHIBE_END_OP()

	/* Temporary register */

	SHIBE_OP(TMOVE) {
		SHIBE_DS_NEED(1);
		state.tp.u32 += SHIBE_DS(0).u32;
		SHIBE_DS_DROP(1);
	} SHIBE_END_OP()

	SHIBE_OP(TSET) {
		SHIBE_DS_NEED(1);
		state.tp = SHIBE_DS(0);
		SHIBE_DS_DROP(1);
	} SHIBE_END_OP()

	SHIBE_OP(TGET) {
		SHIBE_DS_ROOM(1);
		SHIBE_DS_PUSH(state.tp);
	} SHIBE_END_OP()

	SHIBE_OP(TMARK) {
		SHIBE_DS_NEED(1);
		if (SHIBE_DS(0).u32 > state.tm.u32) { state.tm = SHIBE_DS(0); }
		SHIBE_DS_DROP(1);
	} SHIBE_END_OP()

	/* Aux frame */

	SHIBE_OP(ENTER) {
		// The operand cell doubles as the frame's creator: it names the code
		// that built the frame, and the slot count reads back from it
		shibe_cell_t creator = state.ip;
		shibe_cell_t num_slots;
		SHIBE_IMM(num_slots);

		uint64_t frame_len = (uint64_t)SHIBE_AUX_HEADER_LEN + num_slots.u32;
		if ((uint64_t)(as_len - state.asp.u32) < frame_len) {
			SHIBE_FAULT(SHIBE_ERR_STACK_OVERFLOW, SHIBE_STACK_AS);
		}

		uint32_t base = state.asp.u32;
		state.as[base + 0] = state.dsp;
		state.as[base + 1] = state.fp;
		state.as[base + 2] = state.tm;
		state.as[base + 3] = creator;
		state.fp.u32 = base + SHIBE_AUX_HEADER_LEN;
		state.asp.u32 = base + (uint32_t)frame_len;
		// The frame opens with an empty temporary arena, which is what keeps
		// `tp = tm` on LEAVE from freeing the caller's allocations
		state.tm = state.tp;
	} SHIBE_END_OP()

	SHIBE_OP(LEAVE)  { SHIBE_LEAVE_FRAME(false); } SHIBE_END_OP()
	SHIBE_OP(UNWIND) { SHIBE_LEAVE_FRAME(true); } SHIBE_END_OP()

	SHIBE_OP(AGET) {
		shibe_cell_t slot;
		SHIBE_IMM(slot);
		SHIBE_DS_ROOM(1);

		// Slots run from `fp` up to `asp` and the header sits just below `fp`.
		// Anything else is a miscompile, so it reads as zero instead of faulting.
		int64_t index = (int64_t)state.fp.u32 + slot.i32;
		bool in_frame =
			index >= (int64_t)state.fp.u32 - SHIBE_AUX_HEADER_LEN
			&& index >= 0
			&& index < (int64_t)state.asp.u32;
		SHIBE_DS_PUSH(in_frame ? state.as[index] : SHIBE_ZERO);
	} SHIBE_END_OP()

	SHIBE_OP(ASET) {
		shibe_cell_t slot;
		SHIBE_IMM(slot);
		SHIBE_DS_NEED(1);
		shibe_cell_t value = SHIBE_DS(0);
		SHIBE_DS_DROP(1);

		// The header is read only, so a negative slot is dropped on the floor
		int64_t index = (int64_t)state.fp.u32 + slot.i32;
		if (slot.i32 >= 0 && index < (int64_t)state.asp.u32) {
			state.as[index] = value;
		}
	} SHIBE_END_OP()

	/* External call */

	SHIBE_OP(EXTCALL) {
		shibe_cell_t index;
		SHIBE_IMM(index);
		// Call 0 is unbound regardless of whether there is a handler, so an
		// unpatched operand cell always faults instead of dispatching into
		// slot 0
		if (index.u32 == 0 || host->extcall == NULL) {
			SHIBE_FAULT(SHIBE_ERR_UNBOUND, index);
		}

		// Nothing may follow an EXTCALL in its bundle and `ip` is already past
		// the operand cell, so a resume has nothing left to dispatch: it starts
		// on the pseudo opcode and refills, which is also what keeps the call
		// from being made a second time
		SHIBE_HOST_CALL(
			host->extcall(host, vm, index),
			// `vm->state.ip` rather than `state.ip`, because SHIBE_HOST_CALL
			// reads this twice and the register file is only written back to
			// the vm in between.
			SHIBE_ERR_EXTCALL, index, vm->state.ip, true
		);

		SHIBE_NEXT_BUNDLE();
	}

	// Also the catch all: an opcode with no handler is data being run
	SHIBE_OP_DEFAULT(TRAP) {
		SHIBE_FAULT(SHIBE_ERR_TRAP, SHIBE_ZERO);
	}

	SHIBE_END_DISPATCH()
}

#if SHIBE_RELAX_DIAGNOSTICS
#	pragma GCC diagnostic pop
#endif
