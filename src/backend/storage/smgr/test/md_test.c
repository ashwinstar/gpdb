#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"
#include "c.h"

#include "access/appendonlytid.h"
static int access_input[204928];
static int unlink_input[204928];
static int num_unlink_called = 0;
static bool unlink_passing = false;

#undef access
#define access mock_access

int mock_access(const char *path, int amode)
{
	u_int segfile = 0; /* parse the path */
	char tmp[100];
	strncpy(tmp, path, 100);
	char *tmp_path = strtok(tmp, ".");
	if (tmp_path)
	{
		tmp_path = strtok(NULL, ".");
		segfile = atoi(tmp_path);
	}

//	printf("ACCESS %s %d %d\n", path, (segfile - 1)/AOTupleId_MultiplierSegmentFileNum, access_input[segfile]);
	return access_input[segfile];
}

#undef unlink
#define unlink mock_unlink

int mock_unlink(const char * path)
{
	char tmp[100];
	u_int segfile = 0; /* parse the path */
	strncpy(tmp, path, 100);
	char *tmp_path = strtok(tmp, ".");
	if (tmp_path)
	{
		tmp_path = strtok(NULL, ".");
		segfile = atoi(tmp_path);
	}

	num_unlink_called++;

	if (unlink_input[segfile])
		unlink_passing = false;
	else
		unlink_passing = true;

//	printf("UNLINK %s %d num_times_called=%d unlink_passing %d\n",
//		   path, segfile, num_unlink_called, unlink_passing);
}
#include "../md.c"

void
test_mdunlink_co_no_file_exists(void **state)
{
	char path[100] = "/tmp/md_test/1234";
	char segpath[200];

	num_unlink_called = 0;
	memset(access_input, -1, sizeof(access_input));
	mdunlink_co(&path, &segpath);
	assert_true(num_unlink_called == 0);
	return;
}

/* concurrency = 1 max_column = 4 */
void
test_mdunlink_co_4_columns_1_concurrency(void **state)
{
	char path[100] = "/tmp/md_test/1234";
	char segpath[200];

	num_unlink_called = 0;
	memset(access_input, -1, sizeof(access_input));

	/* concurrency 1 exists */
	access_input[1] = 0;

	/* max column exists */
	access_input[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(3*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;

	unlink_input[1] = 0;
	unlink_input[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(3*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;

	mdunlink_co(&path, &segpath);
	assert_true(num_unlink_called == 4);
	assert_true(unlink_passing);
	return;
}

/* concurrency = 1 max_column = 11 */
void
test_mdunlink_co_11_columns_1_concurrency(void **state)
{
	char path[100] = "/tmp/md_test/1234";
	char segpath[200];

	num_unlink_called = 0;
	memset(access_input, -1, sizeof(access_input));
	memset(unlink_input, -1, sizeof(unlink_input));

	/* concurrency 1 exists */
	access_input[1] = 0;

	/* max column exists */
	access_input[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(3*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(4*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(5*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(6*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(7*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(8*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(9*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(10*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;

	unlink_input[1] = 0;
	unlink_input[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(3*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(4*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(5*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(6*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(7*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(8*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(9*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(10*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	
	mdunlink_co(&path, &segpath);
	assert_true(num_unlink_called == 11);
	assert_true(unlink_passing);
	return;
}

/* concurrency = 1,5 max_column = 3 */
void
test_mdunlink_co_3_columns_2_concurrency(void **state)
{
	char path[100] = "/tmp/md_test/1234";
	char segpath[200];

	num_unlink_called = 0;
	memset(access_input, -1, sizeof(access_input));
	memset(unlink_input, -1, sizeof(unlink_input));

	/* concurrency 1,5 exists */
	access_input[1] = 0;
	access_input[5] = 0;

	/* Concurrency 1 files */
	access_input[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	access_input[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;

	/* Concurrency 5 files */
	access_input[(1*AOTupleId_MultiplierSegmentFileNum) + 5] = 0;
	access_input[(2*AOTupleId_MultiplierSegmentFileNum) + 5] = 0;
	
	unlink_input[1] = 0;
	unlink_input[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;
	unlink_input[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = 0;

	unlink_input[5] = 0;
	unlink_input[(1*AOTupleId_MultiplierSegmentFileNum) + 5] = 0;
	unlink_input[(2*AOTupleId_MultiplierSegmentFileNum) + 5] = 0;

	mdunlink_co(&path, &segpath);
	assert_true(num_unlink_called == 6);
	assert_true(unlink_passing);
	return;
}

void
test_mdunlink_co_all_columns_full_concurrency(void **state)
{
	char path[100] = "/tmp/md_test/1234";
	char segpath[200];

	num_unlink_called = 0;
	memset(access_input, 0, sizeof(access_input));
	memset(unlink_input, 0, sizeof(unlink_input));

	mdunlink_co(&path, &segpath);
	assert_true(num_unlink_called == 1600 * AOTupleId_MaxSegmentFileNum);
	assert_true(unlink_passing);
	return;
}

void
test_mdunlink_co_one_columns_one_concurrency(void **state)
{
	char path[100] = "/tmp/md_test/1234";
	char segpath[200];

	num_unlink_called = 0;
	memset(access_input, -1, sizeof(access_input));
	memset(unlink_input, -1, sizeof(unlink_input));

	access_input[1] = 0;
	unlink_input[1] = 0;
	
	mdunlink_co(&path, &segpath);
	assert_true(num_unlink_called == 1);
	assert_true(unlink_passing);
	return;
}

void
test_mdunlink_co_one_columns_full_concurrency(void **state)
{
	char path[100] = "/tmp/md_test/1234";
	char segpath[200];

	num_unlink_called = 0;
	memset(access_input, -1, sizeof(access_input));
	memset(unlink_input, -1, sizeof(unlink_input));

	for (int filenum=1; filenum < MAX_AOREL_CONCURRENCY; filenum++)
	{
		access_input[filenum] = 0;
		unlink_input[filenum] = 0;
	}

	mdunlink_co(&path, &segpath);
	assert_true(num_unlink_called == 127);
	assert_true(unlink_passing);
	return;
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const		UnitTest tests[] = {
		unit_test(test_mdunlink_co_one_columns_full_concurrency),
		unit_test(test_mdunlink_co_one_columns_one_concurrency),
		unit_test(test_mdunlink_co_all_columns_full_concurrency),
		unit_test(test_mdunlink_co_11_columns_1_concurrency),
		unit_test(test_mdunlink_co_3_columns_2_concurrency),
		unit_test(test_mdunlink_co_4_columns_1_concurrency),
		unit_test(test_mdunlink_co_no_file_exists)
	};

	MemoryContextInit();

	return run_tests(tests);
}
