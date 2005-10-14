/*-------------------------------------------------------------------------
 *	cdbtmtest.c
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "postgres.h"
#include "utils/elog.h"
#include "access/xlog_internal.h"
#include "miscadmin.h"
#include "libpq/libpq.h"
#include "storage/ipc.h"

#include "cdb/cdblogsync.h"
#include "cdb/cdbtmlog.h"
#include "cdb/cdbtm.h"


/* for tmlog */
static char tmlogfilename[10];
static int	tmlogfilefd = -1;
static int	tmlogfileoffset = -1;

/* for xlog */
static char xlogfilename[24];
static int	xlogfilefd = -1;
static int	xlogfileoffset = -1;

/* param sent by primary segdb */
static int	cmdtype;
static char wfilename[30];
static int	woffset;
static int	wlen;

static void *buf = NULL;
static int	buflen;
static bool resync = false;

#define MAXPATH  1024

static void cdb_sync_tmlog(void);
static void cdb_sync_xlog(void);
static void parseCmd(const char *);
static void readLogMessage(void *, int);
static int	getLogFileSize(int fd);
static void syncWriteLog(int, void *, int, int);
static void ensureBufferSize(void);
static bool tmlogOpenFile(void);
static bool xlogOpenFile(void);

/*
 * initialize log sync - create recovery.conf file ready for recovery
 */
void
cdb_init_log_sync(void)
{
	/* create recovery.conf file for recovery */
	int			rconffd = -1;
	char		path[MAXPATH];

	/* the first time or file is changed */
	snprintf(path, MAXPGPATH, "%s/recovery.conf", DataDir);
	if (open(path, O_RDWR, 0) < 0)
	{
		char	   *cmd = "restore_command=''\n";

		elog(LOG, "QDSYNC: creating recovery.conf file");
		rconffd = open(path, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (rconffd < 0)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
				 errmsg("QDSYNC: could not create recovery.conf file \"%s\"",
						path)));
		}

		write_with_ereport(rconffd, cmd, strlen(cmd));
	}

	close(rconffd);
}

/*
 * cdb_sync_command - process sync command
 */
void
cdb_sync_command(const char *cmd)
{
	elog(DEBUG3, "QDSYNC: %s", cmd);
	parseCmd(cmd);
	switch (cmdtype)
	{
		case SYNC_XLOG:
			cdb_sync_xlog();
			break;
		case SYNC_TMLOG:
			cdb_sync_tmlog();
			break;
		case SYNC_CHECK:

			/*
			 * check do nothing, this is used to determine postmaster or
			 * mppsyncmaster
			 */
			break;
		case SYNC_CLOSE:
			elog(LOG, "QDSYNC: server is terminated by client");
			break;
	}
	return;
}

/*
 * cdb_sync_tmlog - process tmlog sync
 */
static void
cdb_sync_tmlog(void)
{
	bool		fcreated = tmlogOpenFile();

	if (wlen == 0)
	{
		/* tmlog validation call */
		if (woffset != tmlogfileoffset)
		{
			if (fcreated)
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("QDSYNC: tmlog is not in sync, primary segdb offset: %d, mirrored segdb offset: %d",
								woffset, tmlogfileoffset)));
			else
				ereport(ERROR,
						(errcode_for_file_access(),
				   errmsg("QDSYNC: pdboff: %d, mdboff: %d, resync required.",
						  woffset, tmlogfileoffset)));
		}
	}
	else
	{
		ensureBufferSize();
		readLogMessage(buf, wlen);
		syncWriteLog(tmlogfilefd, buf, woffset, wlen);
		tmlogfileoffset += wlen;
	}

	if (resync)
		elog(LOG, "QDSYNC: resyncing tmlog offset %d len %d", woffset, wlen);
}

/*
 * cdb_sync_xlog - process xlog sync
 */
static void
cdb_sync_xlog(void)
{
	bool		fcreated = xlogOpenFile();

	if (resync && fcreated)
	{
		/*
		 * error case, log file does not exist during resync, previous file
		 * may not have sync completely.
		 */
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("QDSYNC: xlog is not in sync, xlog file does not exist on mirror segdb")));
	}

	/*
	 * no validation checking on xlog. xlog sync is by block and may repeat
	 * the same block, so we do not have any way to check it now. we will rely
	 * on tmlog checking for now.
	 */
	ensureBufferSize();
	readLogMessage(buf, wlen);
	syncWriteLog(xlogfilefd, buf, woffset, wlen);

	if (resync)
		elog(LOG, "QDSYNC: resyncing xlog offset %d len %d", woffset, wlen);
}

/*
 * parse sync command
 */
static void
parseCmd(const char *cmd)
{
	resync = false;
	if (strncmp(cmd, "xlog", 4) == 0)
	{
		/* xlog command */
		cmdtype = SYNC_XLOG;
		sscanf(cmd + 5, "%s %d %d", wfilename, &woffset, &wlen);
	}
	else if (strncmp(cmd, "tmlog", 5) == 0)
	{
		/* tmlog command */
		cmdtype = SYNC_TMLOG;
		sscanf(cmd + 6, "%s %d %d", wfilename, &woffset, &wlen);
	}
	else if (strncmp(cmd, "issyncmaster", 12) == 0)
	{
		cmdtype = SYNC_CHECK;
	}
	else if (strncmp(cmd, "close", 5) == 0)
	{
		cmdtype = SYNC_CLOSE;
	}
	else
		elog(FATAL, "QDSYNC: invalid command '%s'", cmd);

	if (strstr(cmd, "resync") != NULL)
		resync = true;
}

/*
 * readLogMessage - read log message sent from client
 */
static void
readLogMessage(void *buf, int plen)
{
	int32		len;

	if (pq_getbytes((char *) &len, 4) == EOF)
	{
		ereport(FATAL,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("QDSYNC: incomplete log packet")));
	}

	len = ntohl(len);
	len -= 4;
	pq_getbytes(buf, len);
}


/*
 * getLogFileSize
 */
static int
getLogFileSize(int fd)
{
	return lseek(fd, 0, SEEK_END);
}

/*
 * syncWriteLog - sync log
 */
static void
syncWriteLog(int fd, void *buf, int offset, int len)
{
	int			loffset = lseek(fd, offset, SEEK_SET);

	if (loffset != offset)
	{
		elog(PANIC, "QDSYNC: error lseek lo: %d, offset: %d, errno: %d", loffset, offset, errno);
	}
	write_with_ereport(fd, buf, len);
}

/*
 * ensureBufferSize - increment buffer size to hold log message if necessary
 */
static void
ensureBufferSize(void)
{
	if (buf != NULL && buflen < wlen)
	{
		/* buf is smaller, reallocate it */
		free(buf);
		buf = NULL;
	}

	if (buf == NULL)
	{
		buf = malloc(wlen);
		if (buf == NULL)
			elog(PANIC, "QDSYNC: malloc failed in sync tmlog, possiblly running out of memory");
		buflen = wlen;
	}
}

/*
 * open TM log file, return true if file is created.
 */
static bool
tmlogOpenFile(void)
{
	char		path[MAXPATH];
	bool		ret = false;

	if (strcmp(tmlogfilename, wfilename) == 0)
	{
		return ret;
	}

	/* the first time or file is changed */
	snprintf(path, MAXPGPATH, "%s/%s", dtm_dir, wfilename);
	elog(LOG, "QDSYNC: opening tm log file %s", wfilename);
	tmlogfilefd = open(path, O_RDWR, 0);
	if (tmlogfilefd < 0)
	{
		elog(LOG, "QDSYNC: creating tm log file %s", wfilename);
		tmlogfilefd = open(path, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (tmlogfilefd < 0)
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("QDSYNC: could not create tmlog file \"%s\"",
							path)));
		}
		/* skip the first tm file creation */
		if (strcmp(tmlogfilename, "00000000") != 0)
			ret = true;
	}

	strcpy(tmlogfilename, wfilename);
	tmlogfileoffset = getLogFileSize(tmlogfilefd);

	return ret;
}

/*
 * open xlog file
 */
static bool
xlogOpenFile(void)
{
	char		path[MAXPATH];
	bool		ret = false;

	if (strcmp(xlogfilename, wfilename) == 0)
	{
		return ret;
	}

	/* the first time or file is changed */
	snprintf(path, MAXPGPATH, "%s/%s", XLogDir, wfilename);
	elog(LOG, "QDSYNC: opening xlog file %s", wfilename);
	xlogfilefd = open(path, O_RDWR, 0);
	if (xlogfilefd < 0)
	{
		elog(LOG, "QDSYNC: creating xlog file %s", wfilename);
		xlogfilefd = open(path, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (xlogfilefd < 0)
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("QDSYNC: could not create xlog file \"%s\"",
							path)));
		}
		ret = true;
	}

	strcpy(xlogfilename, wfilename);
	xlogfileoffset = getLogFileSize(xlogfilefd);

	return ret;
}
