#include "postgres.h"

#ifndef _POSIX_SOURCE
#include "libc.h"
#else
#include <unistd.h>
#endif

#include <sys/signal.h>


void
putenv(char *name)
{
	extern char **environ;
	static int	was_mallocated = 0;
	int			size;

	/* Compute the size of environ array including the final NULL */
	for (size = 1; environ[size++];)
		 /* nothing */ ;

	if (!was_mallocated)
	{
		char	  **tmp = environ;
		int			i;

		was_mallocated = 1;
		environ = malloc(size * sizeof(char *));

		if(!environ)
			ereport(ERROR, errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("Put env failed: out of memory"));

		for (i = 0; i < size; i++)
			environ[i] = tmp[i];
	}

	environ = realloc(environ, (size + 1) * sizeof(char *));
	if(!environ)
		ereport(ERROR, errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("Put env failed: out of memory"));

	environ[size - 1] = malloc(strlen(name) + 1);
	if(!environ[size - 1])
		ereport(ERROR, errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("Put env failed: out of memory"));

	strcpy(environ[size-1], name);
	environ[size] = NULL;
}

#ifndef _POSIX_SOURCE
int
sigaddset(int *set, int signo)
{
	*set |= sigmask(signo);
	return *set;
}

int
sigemptyset(int *set)
{
	return *set = 0;
}

char *
getcwd(char *buf, size_t size)
{
	return getwd(buf);
}

#endif
