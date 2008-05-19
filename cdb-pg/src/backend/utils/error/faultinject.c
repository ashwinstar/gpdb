/* 
 * faultinject.c
 * 	 Fault injection utilities
 * 
 * Copyright (c) 2008, Greenplum Inc.
 */

#include "postgres.h"
#include "funcapi.h"
#include "utils/faultinjection.h"

#include "cdb/cdbvars.h"

/* exposed in pg_proc.h */
extern Datum gp_fault_inject(PG_FUNCTION_ARGS) ;
Datum gp_fault_inject(PG_FUNCTION_ARGS) 
{
#ifdef USE_FAULT_INJECTION
	int reason = PG_GETARG_INT32(0);
	int arg = PG_GETARG_INT32(1);

	int ret = gp_fault_inject_impl(GP_FAULT_USER, reason, arg, NULL);
	PG_RETURN_INT32(ret);
#else
	PG_RETURN_INT32(0);
#endif
}

#ifdef USE_FAULT_INJECTION
static int user_fault(int reason, int arg)
{
	switch(reason)
	{
		case GP_FAULT_USER_SEGV:
			*(int *) 0 = 1234;
			break;
		case GP_FAULT_USER_LEAK:
			palloc(arg);
			break;
		case GP_FAULT_USER_LEAK_TOP:
			{
				extern MemoryContext TopMemoryContext;
				MemoryContext oldCtxt = MemoryContextSwitchTo(TopMemoryContext);
				palloc(arg);
				MemoryContextSwitchTo(oldCtxt);
			}
			break;
		case GP_FAULT_USER_RAISE_ERROR:
			elog(ERROR, "User fault injection raised error");
		case GP_FAULT_USER_RAISE_FATAL:
			elog(FATAL, "User fault injection raised fatal");
		case GP_FAULT_USER_RAISE_PANIC:
			elog(PANIC, "User fault injection raised panic");

		default:
			elog(ERROR, "Invalid user fault injection code");
	}
	return GP_FAULT_USER;
}
		
static int mp_fault(int reason, int arg, void *argp)
{
	UnusedArg(reason);
	UnusedArg(arg);
	UnusedArg(argp);
	elog(ERROR, "MP fault NYI");
	return GP_FAULT_MP;
}

int gp_fault_inject_impl(int major, int minor, int arg, void *argp)
{
	if((major & gp_fault_injection_flag1) == 0)
		return -1;

	if((minor & gp_fault_injection_flag2) == 0)
		return -2;

	switch(major)
	{
		case GP_FAULT_USER:
			Assert(argp == NULL);
			return user_fault(minor, arg);

		case GP_FAULT_MP:
			return mp_fault(minor, arg, argp);

		default:
			elog(ERROR, "Invalid fault injection major number");
	}

	return 0;
}

#endif

	
