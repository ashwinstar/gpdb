#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "c.h"
#include "postgres.h"
#include "access/aomd.h"

#include "../aomd.c"


void test_aomd_AOSegmentFilePathNameLen(void **state) {
	RelationData reldata;
	char* basepath = "base/21381/123";

	expect_any(relpath, &rnode);
	will_return(relpath, strdup(basepath));

	int r = AOSegmentFilePathNameLen(&reldata);

	assert_in_range(r, strlen(basepath) + 3, strlen(basepath) + 10);
}

void test_aomd_FormatAOSegmentFileName(void **state) {
	char* basepath = "base/21381/123";
	int32 fileSegNo;
	char filepathname[256];

	// seg 0, no columns
	FormatAOSegmentFileName(basepath, 0, -1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123");
	assert_int_equal(fileSegNo, 0);

	// seg 1, no columns
	FormatAOSegmentFileName(basepath, 1, -1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.1");
	assert_int_equal(fileSegNo, 1);

	// seg 0, column 1
	FormatAOSegmentFileName(basepath, 0, 1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.128");
	assert_int_equal(fileSegNo, 128);

	// seg 1, column 1
	FormatAOSegmentFileName(basepath, 1, 1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.129");
	assert_int_equal(fileSegNo, 129);

	// seg 0, column 2
	FormatAOSegmentFileName(basepath, 0, 2, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.256");
	assert_int_equal(fileSegNo, 256);
}


void test_aomd_MakeAOSegmentFileName(void **state) {
	char* basepath = "base/21381/123";
	int32 fileSegNo;
	char filepathname[256];
	RelationData reldata;

	expect_any_count(relpath, &rnode, -1);

	// seg 0, no columns
	will_return(relpath, strdup(basepath));
	MakeAOSegmentFileName(&reldata, 0, -1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123");
	assert_int_equal(fileSegNo, 0);

	// seg 1, no columns
	will_return(relpath, strdup(basepath));
	MakeAOSegmentFileName(&reldata, 1, -1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.1");
	assert_int_equal(fileSegNo, 1);

	// seg 0, column 1
	will_return(relpath, strdup(basepath));
	MakeAOSegmentFileName(&reldata, 0, 1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.128");
	assert_int_equal(fileSegNo, 128);

	// seg 1, column 1
	will_return(relpath, strdup(basepath));
	MakeAOSegmentFileName(&reldata, 1, 1, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.129");
	assert_int_equal(fileSegNo, 129);

	// seg 0, column 2
	will_return(relpath, strdup(basepath));
	MakeAOSegmentFileName(&reldata, 0, 2, &fileSegNo, filepathname);
	assert_string_equal(filepathname, "base/21381/123.256");
	assert_int_equal(fileSegNo, 256);
}


int main(int argc, char* argv[]) {
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] = {
			unit_test(test_aomd_AOSegmentFilePathNameLen),
			unit_test(test_aomd_FormatAOSegmentFileName),
			unit_test(test_aomd_MakeAOSegmentFileName)
	};
	return run_tests(tests);
}
