#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "c.h"
#include "../guc.c"

void test_awesome(void **state) {
	sleep(20);
	SetConfigOption("gp_resqueue_priority_cpucores_per_segment", "32", PGC_POSTMASTER, PGC_S_SESSION);
	printf("%d", gp_resqueue_priority_cpucores_per_segment);
	assert_true(gp_resqueue_priority_cpucores_per_segment == 32);
	SetConfigOption("gp_resqueue_priority_cpucores_per_segment", "33", PGC_POSTMASTER, PGC_S_SESSION);
	printf("%d", gp_resqueue_priority_cpucores_per_segment);
	assert_true(gp_resqueue_priority_cpucores_per_segment == 33);
}

int main(int argc, char* argv[]) {
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test_awesome)
	};
	return run_tests(tests);
}
