#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"
#include "c.h"
#include "utils/elog.h"

#include "postgres.h"
#include "access/appendonlytid.h"
#include "access/appendonlywriter.h"
#include "access/htup.h"

#define PATH_TO_DATA_FILE "/tmp/md_test/1234"
static bool file_present[MAX_AOREL_CONCURRENCY * MaxHeapAttributeNumber];
static int num_unlink_called = 0;
static bool unlink_passing = true;

static void
setup_test_structures()
{
	num_unlink_called = 0;
	memset(file_present, false, sizeof(file_present));
	unlink_passing = true;
}

/*
 *******************************************************************************
 * Mocking access and unlink for unittesting
 *******************************************************************************
 */
#undef access
#define access mock_access

int mock_access(const char *path, int amode)
{
	u_int segfile = 0; /* parse the path */
	char *tmp_path = path + strlen(PATH_TO_DATA_FILE) + 1;
	if (strcmp(tmp_path, "") != 0)
	{
		segfile = atoi(tmp_path);
	}

#if 0
	elog(WARNING, "ACCESS %s %d %d\n", path,
	     (segfile - 1)/AOTupleId_MultiplierSegmentFileNum, file_present[segfile]);
#endif
	return file_present[segfile] ? 0 : -1;
}

#undef unlink
#define unlink mock_unlink

int mock_unlink(const char * path)
{
	u_int segfile = 0; /* parse the path */
	char *tmp_path = path + strlen(PATH_TO_DATA_FILE) + 1;
	if (strcmp(tmp_path, "") != 0)
	{
		segfile = atoi(tmp_path);
	}

	num_unlink_called++;

	if (!file_present[segfile])
		unlink_passing = false;

#if 0
	elog(WARNING, "UNLINK %s %d num_times_called=%d unlink_passing %d\n",
		          path, segfile, num_unlink_called, unlink_passing);
#endif
}
/*
 *******************************************************************************
 */
#include "../md.c"

void
test_mdunlink_co_no_file_exists(void **state)
{
	char segpath[MAXPGPATH];

	setup_test_structures();

	mdunlink_co(PATH_TO_DATA_FILE, &segpath);
	assert_true(num_unlink_called == 0);
	return;
}

/* concurrency = 1 max_column = 4 */
void
test_mdunlink_co_4_columns_1_concurrency(void **state)
{
	char segpath[MAXPGPATH];

	setup_test_structures();

	/* concurrency 1 exists */
	file_present[1] = true;

	/* max column exists */
	file_present[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(3*AOTupleId_MultiplierSegmentFileNum) + 1] = true;

	mdunlink_co(PATH_TO_DATA_FILE, &segpath);
	assert_true(num_unlink_called == 4);
	assert_true(unlink_passing);
	return;
}

/* concurrency = 1 max_column = 11 */
void
test_mdunlink_co_11_columns_1_concurrency(void **state)
{
	char segpath[MAXPGPATH];

	setup_test_structures();

	/* concurrency 1 exists */
	file_present[1] = true;

	/* max column exists */
	file_present[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(3*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(4*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(5*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(6*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(7*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(8*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(9*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(10*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	
	mdunlink_co(PATH_TO_DATA_FILE, &segpath);
	assert_true(num_unlink_called == 11);
	assert_true(unlink_passing);
	return;
}

/* concurrency = 1,5 max_column = 3 */
void
test_mdunlink_co_3_columns_2_concurrency(void **state)
{
	char segpath[MAXPGPATH];

	setup_test_structures();

	/* concurrency 1,5 exists */
	file_present[1] = true;
	file_present[5] = true;

	/* Concurrency 1 files */
	file_present[(1*AOTupleId_MultiplierSegmentFileNum) + 1] = true;
	file_present[(2*AOTupleId_MultiplierSegmentFileNum) + 1] = true;

	/* Concurrency 5 files */
	file_present[(1*AOTupleId_MultiplierSegmentFileNum) + 5] = true;
	file_present[(2*AOTupleId_MultiplierSegmentFileNum) + 5] = true;
	
	mdunlink_co(PATH_TO_DATA_FILE, &segpath);
	assert_true(num_unlink_called == 6);
	assert_true(unlink_passing);
	return;
}

void
test_mdunlink_co_all_columns_full_concurrency(void **state)
{
	char segpath[MAXPGPATH];

	setup_test_structures();

	memset(file_present, true, sizeof(file_present));

	mdunlink_co(PATH_TO_DATA_FILE, &segpath);

	assert_true(num_unlink_called == MaxHeapAttributeNumber * AOTupleId_MaxSegmentFileNum);
	assert_true(unlink_passing);
	return;
}

void
test_mdunlink_co_one_columns_one_concurrency(void **state)
{
	char segpath[MAXPGPATH];

	setup_test_structures();

	file_present[1] = true;

	mdunlink_co(PATH_TO_DATA_FILE, &segpath);
	assert_true(num_unlink_called == 1);
	assert_true(unlink_passing);
	return;
}

void
test_mdunlink_co_one_columns_full_concurrency(void **state)
{
	char segpath[MAXPGPATH];

	setup_test_structures();

	for (int filenum=1; filenum < MAX_AOREL_CONCURRENCY; filenum++)
		file_present[filenum] = true;

	mdunlink_co(PATH_TO_DATA_FILE, &segpath);
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
