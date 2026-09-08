#ifndef SHIBE_ALLOC_H
#define SHIBE_ALLOC_H

#include <barena.h>
#include <shibe.h>

typedef struct {
	shibe_alloc_t base;
	barena_t arena;
} shibe_barena_t;

shibe_alloc_t*
shibe_barena_init(shibe_barena_t* alloc, barena_pool_t* pool);

#endif
