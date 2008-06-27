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
#include "miscadmin.h"
#include "postmaster/memprotserver.h"

/* exposed in pg_proc.h */
extern Datum gp_fault_inject(PG_FUNCTION_ARGS) ;
Datum gp_fault_inject(PG_FUNCTION_ARGS) 
{
#ifdef USE_FAULT_INJECTION
	int64 reason = PG_GETARG_INT32(0);
	int64 arg = PG_GETARG_INT64(1);

	int ret = gp_fault_inject_impl(reason, arg); 
	PG_RETURN_INT64(ret);
#else
	PG_RETURN_INT64(0);
#endif
}

#ifdef USE_FAULT_INJECTION
int64 gp_fault_inject_impl(int32 reason, int64 arg)
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
		case GP_FAULT_USER_PROCEXIT:
			{
				extern void proc_exit(int);
				proc_exit((int) arg);
			}
		case GP_FAULT_USER_ABORT:
			abort();


		case GP_FAULT_USER_CONFIG_MP:
		case GP_FAULT_USER_MP_SEGV:
		case GP_FAULT_USER_MP_ERROR:
		case GP_FAULT_USER_MP_FATAL:
		case GP_FAULT_USER_MP_PANIC:
			{
				bool fOK;
				MemProtectRequest req;
				MemProtectResponse res;

				req.command = MemProt_ConfigFault;
				req.pid = MyProcPid;
				req.reason = reason; 
				req.req_size = arg;

				fOK = MemProtect_Do(&req, &res);
				if(!fOK)
					elog(ERROR, "Config MemProt Server request failed");

				if(res.command == MemProt_Deny)
					elog(ERROR, "Config MemProt Server Denied");

				return res.grant_size;
			}

		default:
			elog(ERROR, "Invalid user fault injection code");
	}
	return 0; 
}
#endif

	
