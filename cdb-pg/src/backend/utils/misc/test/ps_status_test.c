#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "c.h"
#include "../ps_status.c"

/*
 * Check it won't crash in case the ps_buffer overflows.
 */
void
test__set_ps_display(void **state)
{
	ps_buffer = (char *) malloc(64 * sizeof(char));
	memset(ps_buffer, 0x7F, 64 * sizeof(char));
	ps_buffer_fixed_size = 25;
	ps_buffer_size = 32;
	IsUnderPostmaster = true;

	gp_session_id = 1024;
	Gp_role = GP_ROLE_DISPATCH;
	Gp_segment = 24;
	gp_command_count = 1024;
	currentSliceId = 40;

	set_ps_display("testing activity", true);
	set_ps_display("testing activity", false);

	assert_true(ps_buffer[32] == 0x7f);
}

int
main(int argc, char* argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test__set_ps_display)
	};
	return run_tests(tests);
}
