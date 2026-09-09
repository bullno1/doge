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

struct shibe_vm_s {
	shibe_config_t config;
	shibe_state_t state;

	shibe_mem_seg_t regions[8];

	void* snapshot;
};

#endif
