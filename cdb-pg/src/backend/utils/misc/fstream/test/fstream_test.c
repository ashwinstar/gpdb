#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "c.h"
#include "../fstream.c"

/* ========================== find_last_eol_delim ========================== */
static void 
test__find_last_eol_delim__single_char(void **state) 
{
	char *s = "abcdefcg";
	
	char* ans = find_last_eol_delim(s, strlen(s), "c", 1);
	assert_string_equal(ans, "cg");
}

static void 
test__find_last_eol_delim__multi_chars(void **state) 
{
	char *s = "abcdefgcdh";
	
	char* ans = find_last_eol_delim(s, strlen(s), "cd", 2);
	assert_string_equal(ans, "cdh");
}

static void 
test__find_last_eol_delim__multi_chars_at_the_beginning(void **state) 
{
	char *s = "abcdefgh";
	
	char* ans = find_last_eol_delim(s, strlen(s), "ab", 2);
	assert_string_equal(ans, "abcdefgh");
}

static void 
test__find_last_eol_delim__multi_chars_at_the_end(void **state) 
{
	char *s = "abcdefgcdhcd";
	
	char* ans = find_last_eol_delim(s, strlen(s), "cd", 2);
	assert_string_equal(ans, "cd");
}

static void 
test__find_last_eol_delim__multi_chars_with_null(void **state) 
{
	char *s = "abc\0defg";
	
	char* ans = find_last_eol_delim(s, 10, "c\0d", 3);
	assert_memory_equal(ans, "c\0defg", 6);
}

static void 
test__find_last_eol_delim__not_exists(void **state) 
{
	char *s = "abcdefg";
	
	char* ans = find_last_eol_delim(s, strlen(s), "hi", 2);
	assert_true(ans == NULL);
}


/* ========================== main ========================== */
int 
main(int argc, char *argv[]) 
{
	cmockery_parse_arguments(argc, argv);
	
	const UnitTest tests[] = {
		unit_test(test__find_last_eol_delim__single_char),
		unit_test(test__find_last_eol_delim__multi_chars),
		unit_test(test__find_last_eol_delim__multi_chars_at_the_beginning),
		unit_test(test__find_last_eol_delim__multi_chars_at_the_end),
		unit_test(test__find_last_eol_delim__multi_chars_with_null),
		unit_test(test__find_last_eol_delim__not_exists)
	};
	
	return run_tests(tests);
}
