#ifndef SHIBE_INTERNAL_H
#define SHIBE_INTERNAL_H

#include <shibe.h>

#define SHIBE_ZERO ((shibe_cell_t){ 0 })

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
