#include <btest.h>
#include <shibe.h>
#include "common.h"

static btest_suite_t basic = {
	.name = "shibe/basic",

	.init_per_suite = init_per_suite,
	.cleanup_per_suite = cleanup_per_suite,
	.init_per_test = init_per_test,
	.cleanup_per_test = cleanup_per_test,
};

BTEST(basic, create_destroy) {
	shibe_vm_t* vm = shibe_create((shibe_config_t){
		.alloc = shibe_alloc,
	});

	BTEST_EXPECT(vm != NULL);

	shibe_destroy(vm);
}
