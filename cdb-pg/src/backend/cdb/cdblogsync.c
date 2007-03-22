/*-------------------------------------------------------------------------
 *	cdbtmtest.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define PROVIDE_64BIT_CRC


#include "utils/elog.h"
#include "access/xlog_internal.h"
#include "miscadmin.h"
#include "libpq/libpq.h"
#include "storage/ipc.h"

#include "cdb/cdblogsync.h"
#include "cdb/cdbtm.h"

/*
 * There is a protocol consisting of four messages from primary->mirror:
 *
 * Q xlog filename woffset wlen [resync]
 * Q issyncmaster [resync]
 * Q close [resync]
 */

/* for xlog */
static char xlogfilename[100];
static int	xlogfilefd = -1;
static int	xlogfileoffset = -1;

/* param sent by primary segdb */
static int	cmdtype;
static char wfilename[85];
static int	woffset;
static int	wlen;

/* XXX: file global buffer, called "buf" ??? At least it is allocated and free()ed as needed. */
static void *buf = NULL;
static int	buflen;
static bool resync = false;

static void cdb_sync_xlog(void);
static void parseCmd(const char *);
static void readLogMessage(void *, int);
static int	getLogFileSize(int fd);
static void syncWriteLog(int, void *, int, int);
static void ensureBufferSize(void);
static bool xlogOpenFile(void);
static void write_with_ereport(int fd, void *data, int len);


static void
write_with_ereport(int fd, void *data, int len)
{
	int			write_len;

	write_len = write(fd, data, len);

	if (write_len != len)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("error writing file: %m")));
	}
}


/*
 * initialize log sync - create recovery.conf file ready for recovery
 */
void
cdb_init_log_sync(void)
{
	/* create recovery.conf file for recovery */
	int			rconffd = -1;
	char		path[MAXPGPATH];

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
	elog(LOG, "QDSYNC: %s", cmd);
	parseCmd(cmd);

	getTmLock();

	switch (cmdtype)
	{
		case SYNC_XLOG:
			cdb_sync_xlog();
			break;
		case SYNC_CHECK:
			/*
			 * check do nothing, this is used to determine postmaster or
			 * gpsyncmaster
			 */
			break;
		case SYNC_CLOSE:
			elog(LOG, "QDSYNC: server is terminated by client");
			break;
	}
	releaseTmLock();

	return;
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

	elog(DEBUG4, "cdb_sync_xlog: woffset %d len %d", woffset, wlen);

	syncWriteLog(xlogfilefd, buf, woffset, wlen);

	if (resync)
		elog(LOG, "QDSYNC: resyncing xlog offset %d len %d", woffset, wlen);
}

/*
 * We parse incoming commands into a file-local static set of parameters.
 */
static void
getCmdArgs(const char *cmdArgs)
{
	char	   *pos;
	int			len;

	do
	{
		pos = strchr(cmdArgs, ' ');
		if (pos == NULL)
			break;

		len = pos - cmdArgs;
		if (len > sizeof(wfilename) - 1)
			break;

		strncpy(wfilename, cmdArgs, len);
		wfilename[len] = 0;

		pos++;
		woffset = atoi(pos);

		pos = strchr(pos, ' ');
		if (pos == NULL)
			break;

		pos++;
		wlen = atoi(pos);

		return;
	} while (0);

	elog(ERROR, "gpsyncmaster: Cannot parse cmd args '%s'", cmdArgs);
	return;
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
		getCmdArgs(cmd + strlen("xlog") + 1);
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
		ereport(FATAL,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("QDSYNC: invalid command '%s'", cmd),
				 errhint("Failover-QD needs to be 'activated' before issuing queries")));

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

	elog(DEBUG4, "readLogMessage: plen %d len: %d", plen, len);
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
 * open xlog file
 */
static bool
xlogOpenFile(void)
{
	char		path[MAXPGPATH];
	bool		ret = false;

	elog(DEBUG3, "QDSYNC: opening xlog file %s", wfilename);

	if (strcmp(xlogfilename, wfilename) == 0)
	{
		return ret;
	}

	elog(DEBUG1, "opening xlog file %s", wfilename);

	/* the first time or file is changed */
	if (snprintf(path, MAXPGPATH, "%s/%s", XLOGDIR, wfilename) >= MAXPGPATH)
	{
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("QDSYNC: could not create xlog file \"%s/%s\"",
						XLOGDIR, wfilename)));
	}

	if (xlogfilefd >= 0)
	{
		close(xlogfilefd);
		xlogfilefd = -1;
		xlogfileoffset = -1;
	}

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
