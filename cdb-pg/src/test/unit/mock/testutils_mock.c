/*
 * Auto-generated Mocking Source
 * 
 * Source: utils/testutils.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include <sys/time.h>
#include <sys/resource.h>
#include "postgres.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "utils/elog.h"
#include "utils/testutils.h"

volatile int32  InterruptHoldoffCount;
volatile int32  CritSectionCount;
void 
TimeSliceReset(void)
{



    mock();

}

void 
TimeSliceCheck(const char *file, int line)
{

    check_expected(file);
    check_expected(line);

    mock();

}

void 
LWLockHeldDetect(const void *locktag, int lockmode)
{

    check_expected(locktag);
    check_expected(lockmode);

    mock();

}

bool            gp_test_time_slice;
int             gp_test_time_slice_interval;
int             gp_test_time_slice_report_level;
bool            gp_test_deadlock_hazard;
int             gp_test_deadlock_hazard_report_level;
