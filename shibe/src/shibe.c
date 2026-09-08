#include "internal.h"

shibe_vm_t*
shibe_create(shibe_config_t config) {
	void* snapshot = shibe_snapshot(&config.alloc);
	shibe_vm_t* vm = shibe_alloc(&config.alloc, sizeof(shibe_vm_t), _Alignof(shibe_vm_t));
	*vm = (shibe_vm_t){
		.config = config,
		.snapshot = snapshot,
	};

	return vm;
}

void
shibe_destroy(shibe_vm_t* vm) {
	shibe_restore(&vm->config.alloc, vm->snapshot);
}
