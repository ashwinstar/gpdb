#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "c.h"
#include "../memmem.c"

/* ========================== memmem ========================== */
static void 
test__memmem__single_char(void **state) 
{
	char *s = "abcdefg";
	
	char* ans = memmem(s, strlen(s), "c", 1);
	assert_string_equal(ans, "cdefg");
}

static void 
test__memmem__multi_chars(void **state) 
{
	char *s = "abcdefgcdh";
	
	char* ans = memmem(s, strlen(s), "cd", 2);
	assert_string_equal(ans, "cdefgcdh");
}

static void 
test__memmem__multi_chars_with_null(void **state) 
{
	char *s = "abc\0defg";
	
	char* ans = memmem(s, 8, "c\0d", 3);
	assert_memory_equal(ans, "c\0defg", 6);
}


/* ========================== main ========================== */
int 
main(int argc, char *argv[]) 
{
	cmockery_parse_arguments(argc, argv);
	
	const UnitTest tests[] = {
		unit_test(test__memmem__single_char),
		unit_test(test__memmem__multi_chars),
		unit_test(test__memmem__multi_chars_with_null)
	};
	
	return run_tests(tests);
}
