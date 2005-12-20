/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert code.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
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

#include <unistd.h>

#if (!defined(__darwin__)) && (!defined(__solaris__))
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif

/*
 * ExceptionalCondition - Handles the failure of an Assert()
 */
int
ExceptionalCondition(char *conditionName,
					 char *errorType,
					 char *fileName,
					 int lineNumber)
{
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName)
		|| !PointerIsValid(errorType))
		write_stderr("TRAP: ExceptionalCondition: bad arguments\n");
	else
	{
		write_stderr("TRAP: %s(\"%s\", File: \"%s\", Line: %d)\n",
					 errorType, conditionName,
					 fileName, lineNumber);
	}
	
#if (!defined(__darwin__)) && (!defined(__solaris__))
#if defined(__GLIBC__)
		
	{
		void	   *array[10];
		size_t		size;
		char	  **strings;
		size_t		i;
	
		size = backtrace(array, 10);
		strings = backtrace_symbols(array, size);
		for (i = 0; i < size; i++)
		{
			write_stderr("%s\n",strings[i]);
		}
		free(strings);
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

	abort();

	return 0;
}
