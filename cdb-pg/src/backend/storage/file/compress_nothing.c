/* compress_nothing.c */

#include "postgres.h"

#include "c.h"
#include <unistd.h>
#include <storage/bfz.h>
#include <utils/memutils.h>

/*
 * This file implements bfz compression algorithm "nothing".
 * We don't compress at all.
 */

static void
bfz_nothing_close_ex(bfz_t * thiz)
{
	pfree(thiz->freeable_stuff);
	thiz->freeable_stuff = NULL;
}

static int
bfz_nothing_read_ex(bfz_t * thiz, char *buffer, int size)
{
	int			orig_size = size;

	while (size)
	{
		int			i = readAndRetry(thiz->fd, buffer, size);

		if (i < 0)
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					errmsg("could not read from temporary file: %m")));
		if (i == 0)
			break;
		buffer += i;
		size -= i;
	}
	return orig_size - size;
}

static void
bfz_nothing_write_ex(bfz_t * bfz, const char *buffer, int size)
{
	while (size)
	{
		int			i = writeAndRetry(bfz->fd, buffer, size);

		if (i < 0)
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					errmsg("could not write to temporary file: %m")));
		buffer += i;
		size -= i;
	}
}

void
bfz_nothing_init(bfz_t * thiz)
{
	/* Allocate this in TopMemoryContext since it needs to be alive during cleanup */
	struct bfz_freeable_stuff *fs = MemoryContextAlloc(TopMemoryContext, sizeof *fs);

	if (!fs)
		ereport(ERROR,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory")));

	thiz->freeable_stuff = fs;

	fs->read_ex = bfz_nothing_read_ex;
	fs->write_ex = bfz_nothing_write_ex;
	fs->close_ex = bfz_nothing_close_ex;
}
