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

#include "postmaster/syslogger.h"

#include <unistd.h>

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

static char * longmsg = 
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
"12345678901234567890123456789012345678901234567890"
;

static char *multiline = "1234567890\n1234567890\n";
static char *quotestr = "\"\"\nXXX\nYYY\"\"\n\r\n\"\tZZZ\t\"\"\\\\\"";

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

		case GP_FAULT_USER_INFINITE_LOOP:
			{
				do {
					/* do nothing */
				} while(true);
			}
			break;

		case GP_FAULT_USER_MP_CONFIG:
		case GP_FAULT_USER_MP_ALLOC:
		case GP_FAULT_USER_MP_HIGHWM:
		case GP_FAULT_SEG_AVAILABLE:
		case GP_FAULT_SEG_SET_VMEMMAX:
		case GP_FAULT_SEG_GET_VMEMMAX:
			return gp_mp_fault(reason, arg); 

                case GP_FAULT_LOG_LONGMSG:
                        elog(LOG, 
                                        "%s%s%s%s%s" "%s%s%s%s" "%s%s%s%s%s", 
                                        longmsg, longmsg, longmsg, longmsg, longmsg,
                                        multiline, multiline, quotestr, quotestr,
                                        longmsg, longmsg, longmsg, longmsg, longmsg
                            );
                        break;
                case GP_FAULT_LOG_3RDPARTY:
                        fprintf(stderr, "Hello from 3rd party");
						fflush(stderr);
                        break;

                case GP_FAULT_LOG_3RDPARTY_LONGMSG:
                        fprintf(stderr,  
                                        "%s%s%s%s%s" "%s%s%s%s" "%s%s%s%s%s", 
                                        longmsg, longmsg, longmsg, longmsg, longmsg,
                                        multiline, multiline, quotestr, quotestr,
                                        longmsg, longmsg, longmsg, longmsg, longmsg
                               );
			fflush(stderr);
                        break;
                        
                case GP_FAULT_LOG_CRASH:
                        {
                                PipeProtoHeader hdr;
                                hdr.zero = 0;
                                hdr.len = 0;
                                hdr.pid = 1; 
                                hdr.thid = 1; 
                                hdr.main_thid = 1;
                                hdr.chunk_no = 0;
                                hdr.is_last = 't';
                                hdr.log_format = 'X';
                                hdr.log_line_number = reason;
                                hdr.next = -1;
                                write(2, &hdr, sizeof(PipeProtoHeader));
                         }
                         break;
		default:
			elog(ERROR, "Invalid user fault injection code");
	}
	return 0; 
}
#endif

	
