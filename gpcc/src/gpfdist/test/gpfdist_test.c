#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"
#include "c.h"
#include "../gpfdist_helper.c"

void test__simple(void **state);
void test__is_valid_timeout(void **state);
void test__is_valid_listen_queue_size(void **state);

void 
test__is_valid_timeout(void **state)
{
	assert_false(is_valid_timeout(-1));
	assert_true(is_valid_timeout(0));
	assert_false(is_valid_timeout(1));
	assert_true(is_valid_timeout(2));
	assert_true(is_valid_timeout(20));
	assert_true(is_valid_timeout(600));
	assert_false(is_valid_timeout(601));
}

void
test__is_valid_listen_queue_size(void **state)
{
	assert_false(is_valid_listen_queue_size(-1));
	assert_false(is_valid_listen_queue_size(0));
	assert_false(is_valid_listen_queue_size(15));
	assert_true(is_valid_listen_queue_size(16));
	assert_true(is_valid_listen_queue_size(17));
	assert_true(is_valid_listen_queue_size(511));
	assert_true(is_valid_listen_queue_size(512));
	assert_false(is_valid_listen_queue_size(513));
}



int 
main(int argc, char* argv[]) 
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test__is_valid_timeout),
			unit_test(test__is_valid_listen_queue_size)
	};
	return run_tests(tests);
}
