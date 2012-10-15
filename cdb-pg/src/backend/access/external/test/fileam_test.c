#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "c.h"
#include "../fileam.c"

/* ========================== base16_encode ========================== */
static void 
test__base16_encode__regular_chars(void **state) 
{
	char *raw = "a3 ";
	int len = strlen(raw);
	char encoded[len * 2 + 1];
	
	base16_encode(raw, len, encoded);
	assert_string_equal(encoded, "613320");
}

static void 
test__base16_encode__null_char(void **state) 
{
	char *raw = "\0a";
	int len = 2;
	char encoded[len * 2 + 1];
	
	base16_encode(raw, len, encoded);
	assert_string_equal(encoded, "0061");
}

/* ========================== get_eol_delimiter ========================== */
static void 
test__get_eol_delimiter__exists(void **state) 
{
	List *l = NIL;
	char *eol;
	
	l = lappend(l, makeDefElem("dummy1", (Node *)makeString("dummy1")));
	l = lappend(l, makeDefElem("dummy2", (Node *)makeString("dummy2")));
	l = lappend(l, makeDefElem("line_delim", (Node *)makeString("eol@@ ")));
	l = lappend(l, makeDefElem("dummy3", (Node *)makeString("dummy3")));
	
	eol = get_eol_delimiter(l);
	assert_string_equal(eol, "eol@@ ");
}

static void 
test__get_eol_delimiter__not_exists(void **state) 
{
	List *l = NIL;
	char *eol;
	
	l = lappend(l, makeDefElem("dummy1", (Node *)makeString("dummy1")));
	l = lappend(l, makeDefElem("dummy2", (Node *)makeString("dummy2")));
	l = lappend(l, makeDefElem("dummy3", (Node *)makeString("dummy3")));
	
	eol = get_eol_delimiter(l);
	assert_true(eol == NULL);
}

static void 
test__get_eol_delimiter__empty_list(void **state) 
{
	List *l = NIL;
	char *eol = get_eol_delimiter(l);
	
	assert_true(eol == NULL);
}


/* ========================== main ========================== */
int 
main(int argc, char* argv[]) 
{
	cmockery_parse_arguments(argc, argv);
	
	const UnitTest tests[] = {
		unit_test(test__base16_encode__regular_chars),
		unit_test(test__base16_encode__null_char),
		unit_test(test__get_eol_delimiter__exists),
		unit_test(test__get_eol_delimiter__not_exists),
		unit_test(test__get_eol_delimiter__empty_list)
	};
	
	return run_tests(tests);
}
