#ifndef SHIBE_ALLOC_H
#define SHIBE_ALLOC_H

#include <barena.h>
#include <shibe.h>

typedef struct {
	shibe_allocator_t impl;
	barena_t arena;
} shibe_barena_t;

shibe_allocator_t*
shibe_barena_init(shibe_barena_t* alloc, barena_pool_t* pool);

#endif
