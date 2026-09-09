#ifndef SHIBE_INTERNAL_H
#define SHIBE_INTERNAL_H

#include <stdbool.h>
#include <shibe.h>

#define SHIBE_ZERO ((shibe_cell_t){ 0 })

// A SHIBE_ERR_STACK_* panic carries which stack it was about in its `arg`
#define SHIBE_STACK_DS ((shibe_cell_t){ .u32 = 0 })
#define SHIBE_STACK_AS ((shibe_cell_t){ .u32 = 1 })

// The frame header is saved_dsp, saved_fp, saved_tm, creator, in that order.
// `fp` points just past it, so the four sit at fp-4 .. fp-1, which is what the
// negative AGET indices in shibe/opcode.h address.
#define SHIBE_AUX_HEADER_LEN 4

// A host frame lays three cells under the standard header:
//
// - Where the run that called out resumes. A CALL would have left its return
//   address in the same place. 0 means there is no run underneath, which is
//   what a frame opened by a top level call looks like.
// - The extcall number that finishes the call after a suspension, 0 for none.
// - How many slots follow, so a host accessor can bound them.
#define SHIBE_AUX_HOST_HEADER_LEN (3 + SHIBE_AUX_HEADER_LEN)

// Offsets from `fp`. The standard header occupies -1 .. -4, so the three cells
// belonging to a host frame sit below it and out of AGET's reach.
#define SHIBE_AUX_HOST_NUM_LOCALS    (-5)
#define SHIBE_AUX_HOST_CONTINUATION (-6)
#define SHIBE_AUX_HOST_OUTER_IP     (-7)

#define BSEG_API static inline
// Number of doubling segments that fit in the SHIBE_MEM_INDEX_BITS index space.
// Segment 0 is 2^BSEG_SKIPPED_SEGMENTS elements, so:
// BSEG_MAX_SEGMENTS = 29 - BSEG_SKIPPED_SEGMENTS
#define BSEG_MAX_SEGMENTS 23
#include <bseg.h>
#define SHIBE_REGION_MAX_LEN (((size_t)1u << SHIBE_MEM_INDEX_BITS) - ((size_t)1u << 6))

typedef bseg(shibe_cell_t) shibe_mem_seg_t;

struct shibe_vm_s {
	shibe_config_t config;
	shibe_state_t state;

	shibe_mem_seg_t regions[8];

	// The frame of the host call that is running, or 0 when none is running or
	// the one that is did not ask for a frame. Frames nest with the C stack, so
	// every callback site saves it and puts it back.
	uint32_t hfp;

	// Whether the host call that is running is one a suspension can come back to.
	// Everything a suspension is written down against is an address, which can
	// only address anything at a bundle boundary.
	// While EXTCALL ends its bundle, the hook stops in the middle of one, so it
	// only qualifies at slot 0, where nothing in the bundle has run yet.
	bool can_suspend;

	// Where the run underneath the host call that is running carries on. For an
	// EXTCALL that is `ip` itself, since nothing may follow one in its bundle;
	// for the debug hook it is the bundle being reported, which `ip` has already
	// moved past. Kept beside the register file rather than in it so that the
	// hook is shown the vm as it really is.
	shibe_cell_t resume_ip;

	// Set when a callback stopped after whatever it ran had already finished.
	// There is no run to pick up in that case: the resume starts by finishing
	// that callback's frame through its continuation.
	bool at_continuation;

	void* snapshot;
};

#endif
