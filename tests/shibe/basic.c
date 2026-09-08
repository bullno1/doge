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
}
