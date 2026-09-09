#ifndef SHIBE_INTERNAL_H
#define SHIBE_INTERNAL_H

#include <shibe.h>

#define SHIBE_ZERO ((shibe_cell_t){ 0 })

// A SHIBE_ERR_STACK_* panic carries which stack it was about in its `arg`
#define SHIBE_STACK_DS ((shibe_cell_t){ .u32 = 0 })
#define SHIBE_STACK_AS ((shibe_cell_t){ .u32 = 1 })

// The frame header is saved_dsp, saved_fp, saved_tm, creator, in that order.
// `fp` points just past it, so the four sit at fp-4 .. fp-1, which is what the
// negative AGET indices in shibe/opcode.h address.
#define SHIBE_AUX_HEADER_LEN 4

// A re-entry from the host lays down the outer run's `ip` where a CALL would
// have left its return address, then a header of its own on top of it
#define SHIBE_AUX_REENTRY_LEN (1 + SHIBE_AUX_HEADER_LEN)

#define BSEG_API static inline
// Number of doubling segments that fit in the SHIBE_MEM_INDEX_BITS index space.
// Segment 0 is 2^BSEG_SKIPPED_SEGMENTS elements, so:
// BSEG_MAX_SEGMENTS = 29 - BSEG_SKIPPED_SEGMENTS
#define BSEG_MAX_SEGMENTS 23
#include <bseg.h>
#define SHIBE_REGION_MAX_LEN (((size_t)1u << SHIBE_MEM_INDEX_BITS) - ((size_t)1u << 6))

typedef bseg(shibe_cell_t) shibe_mem_seg_t;

// What a suspended run needs handed back that the register file does not carry:
// where the interpreter was inside the bundle it was decoding.
typedef struct {
	// The decode window as the resumed run should see it. A run the debug hook
	// suspended puts the opcode it stopped on back in, so that opcode is the
	// first thing the resume dispatches. A run an EXTCALL suspended stores the
	// bootstrap value instead: EXTCALL ends its bundle, so there is nothing
	// left to dispatch and the resume refills from `ip`.
	uint64_t win;
	// Where the debug hook had got to. Meaningless without a hook, and
	// overwritten by the first refill, which is what every EXTCALL suspension
	// starts with.
	shibe_op_addr_t at;
} shibe_suspension_t;

struct shibe_vm_s {
	shibe_config_t config;
	shibe_state_t state;

	shibe_mem_seg_t regions[8];

	shibe_suspension_t suspension;

	void* snapshot;
};

#endif
