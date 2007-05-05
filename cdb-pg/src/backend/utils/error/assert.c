/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert code.
 *
 * Portions Copyright (c) 2005-2006, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/error/assert.c,v 1.31 2005/10/15 02:49:32 momjian Exp $
 *
 * NOTE
 *	  This should eventually work with elog()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/elog.h" /* backtrace() prototype for solaris */
#include <unistd.h>

#if (!defined(__darwin__)) && (!defined(pg_on_solaris))
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif


/*
 * ExceptionalCondition - Handles the failure of an Assert()
 */
int
ExceptionalCondition(const char *conditionName,
					 const char *errorType,
					 const char *fileName,
					 int         lineNumber)
{
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName)
		|| !PointerIsValid(errorType))
		write_stderr("TRAP: ExceptionalCondition: bad arguments\n");
	else
	{
		extern int currentSliceId;
		write_stderr("TRAP: %s(\"%s\", File: \"%s\", Line: %d, sliceNum %d)\n",
					 errorType, conditionName,
					 fileName, lineNumber, currentSliceId);
	}
    fflush(stdout);
	
#if (!defined(__darwin__))
#if defined(__GLIBC__) || defined(pg_on_solaris)
		
	{
		void	   *array[20];
		size_t		size;
		char	  **strings;
		size_t		i;
	
		size = backtrace(array, 20);
		if (size > 0)
		{
			strings = backtrace_symbols(array, size);
			for (i = 0; i < size; i++)
			{
				write_stderr("%s\n",strings[i]);
			}
		}
	}
#endif
#endif

#ifdef SLEEP_ON_ASSERT 
	/*
	 * It would be nice to use pg_usleep() here, but only does 2000 sec or 33
	 * minutes, which seems too short.
	 */
	sleep(1000000);
#endif

#if 0
	{
		extern void debug_break();
		debug_break();
	}
#else
    /* Assert should always be painful.  Force a segment fault.
     * For reasons, cdb2 does not dump a core without the following
     * hack.
     */
	{
		*( (int *) NULL ) = 1234;
	}
#endif

    /* CDB: Try to tell the QD or client what happened. */
    if (errstart(FATAL, fileName, lineNumber, NULL))
    {
        errfinish(errcode(ERRCODE_INTERNAL_ERROR),
                  errmsg("Unexpected internal error"),
                  errdetail("%s(\"%s\", File: \"%s\", Line: %d)\n",
				            errorType, conditionName, fileName, lineNumber)
                  );
        /* not reached */
    }

    abort();

	return 0;
}
