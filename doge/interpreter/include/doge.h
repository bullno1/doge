#ifndef DOGE_H
#define DOGE_H

#include <shibe.h>

#ifndef DOGE_API
#define DOGE_API
#endif

typedef struct doge_s doge_t;

typedef struct {
	uint32_t len;
	const char* chars;
} doge_str_t;

typedef struct doge_host_s doge_host_t;
struct doge_host_s {
	void (*panic)(doge_host_t* host, doge_t* doge, const shibe_panic_t* panic);
	shibe_status_t (*debug)(doge_host_t* host, doge_t* doge, const shibe_state_t* state, shibe_op_addr_t at);
	shibe_status_t (*extcall)(doge_host_t* host, doge_t* doge, shibe_cell_t index);
};

typedef struct {
	shibe_config_t shibe_config;
	doge_host_t* host;
} doge_config_t;

typedef struct { shibe_cell_t id; } doge_type_t;

typedef struct { shibe_cell_t id; } doge_export_t;

typedef struct {
	doge_type_t type;
	shibe_cell_t cell;
} doge_value_t;

typedef struct {
	uint32_t line;
	uint32_t col;
	uint32_t offset;
} doge_source_pos_t;

typedef struct {
	doge_source_pos_t from;  // Inclusive
	doge_source_pos_t to;  // Exclusive
} doge_source_span_t;

typedef struct {
	doge_str_t file;
	doge_source_span_t span;
} doge_source_region_t;

DOGE_API doge_t*
doge_create(doge_config_t config);

DOGE_API void
doge_destroy(doge_t* doge);

DOGE_API void
doge_destroy(doge_t* doge);

DOGE_API void
doge_begin_interpret(doge_t* doge, bool interactive);

DOGE_API void
doge_set_source_location(
	doge_t* doge,
	doge_str_t file,
	doge_source_pos_t position
);

DOGE_API void
doge_interpret(doge_t* doge, doge_str_t chunk);

DOGE_API void
doge_end_interpret(doge_t* doge);

DOGE_API void
doge_reset(doge_t* doge);

DOGE_API bool
doge_find_type(doge_t* doge, doge_str_t name, doge_type_t* out);

DOGE_API bool
doge_find_export(doge_t* doge, doge_str_t name, doge_export_t* out);

DOGE_API void
doge_push(doge_t* doge, doge_value_t value);

DOGE_API doge_value_t
doge_pop(doge_t* doge);

DOGE_API shibe_status_t
doge_execute(doge_t* doge, doge_export_t export);

// Escape hatch until we find a way to make everything type-safe.
// Direct VM manipulation could break the type checker's assumption.
DOGE_API shibe_vm_t*
doge_get_vm(doge_t* doge);

#endif
