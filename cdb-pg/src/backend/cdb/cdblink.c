/*-------------------------------------------------------------------------
 *
 * cdblink.c
 *
 *	  Setting up libpq connections to qRxecs from Dispatcher
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#define PROVIDE_64BIT_CRC
#include "postgres.h"

#include "libpq-fe.h"
#include "libpq-int.h"
#include "pqexpbuffer.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "nodes/makefuncs.h"
#include "commands/dbcommands.h"
#include "access/xlog.h"

#include "cdb/cdbcat.h"
#include "cdb/cdbconn.h"
#include "cdb/cdblink.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbfts.h"


/*
 * Points to the SegmentDatabaseDescriptor of mirrored entry database
 */
static SegmentDatabaseDescriptor *cdb_mirrored_entry_db = NULL;
static bool resync = false;

static bool cdblink_connectQD(SegmentDatabaseDescriptor * segdbDesc);

/*
 * Called by backend/utils/init/postinit.c to setup stuff
 *
 * Note -- this function is called with TopTransactionContext *before*
 *		MessageContext and the other statement-oriented contexts are
 *		allocated.	We want to store the SegmentDbArray and the
 *		heap returned by getCdbSegmentInstances() in TopMemoryContext
 *		(basically permanent storage).	TopTransactionContext is
 *		reset shortly after this routine is called.
 */

void
cdblink_setup(void)
{
	/*
	 * If gp_role is UTILITY, skip this call.
	 */
	if (Mpp_role == MPP_ROLE_UTILITY)
		return;

	getGpId();

	return;
}

#if 0
/* Alas, Darwin doesn't have this very useful facility. */
/* Solaris 10 doesn't either... But how to know we are on that? */
#if !defined(__darwin__)

#include <execinfo.h>

static void
log_backtrace(void)
{
	void	   *array[10];
	size_t		size;
	char	  **strings;
	size_t		i;

	size = backtrace(array, 10);
	strings = backtrace_symbols(array, size);
	for (i = 0; i < size; i++)
		elog(DEBUG3, "%s\n", strings[i]);
	free(strings);
}

#else
/* assume that we're using GNU compiler */
#define log_backtrace() elog(DEBUG3, "called by %p", __builtin_return_address(0))
#endif
#endif

/*
 * Returns the number of segments
 *
 * N.B.  Mpp_role must be either dispatch or execute, since
 * when utiliy	no GP catalog tables are read.  An Assert is
 * thrown if Mpp_role = utility.
 */
int
getgpsegmentCount(void)
{
	if (Mpp_role == MPP_ROLE_UTILITY)
	{
		if (MppIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getgpsegmentCount called when Mpp_role == utility. returning zero segments.");
			return 0;
		}

		elog(DEBUG1, "getgpsegmentCount called when Mpp_role == utility, but is relying on gp_id info");
		/* log_backtrace(); */

	}

	if (Mpp_role == MPP_ROLE_EXECUTE)
	{
		if (MppIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getgpsegmentCount called when Mpp_role == execute. returning zero segments.");
			return 0;
		}
		elog(DEBUG1, "getgpsegmentCount called when Mpp_role == execute, but is relying on gp_id info");
		/* log_backtrace(); */
	}
	/* Assert( Mpp_role != MPP_ROLE_EXECUTE ); */

	if (MppIdentity.numsegments <= 0)
	{
		elog(LOG, "getgpsegmentCount called BEFORE gp_id record has been read in.");
		/* log_backtrace(); */
	}

	/* Assert( Mpp_role != MPP_ROLE_UTILITY ); */

	return MppIdentity.numsegments;
}

/*
 * Now we connect to mirrored entry db.
 */
void
buildMirrorQDDefinition(void)
{
	CdbComponentDatabases		*cdb_component_dbs;

	if (!isQDMirroringEnabled())
		return;

	cdb_component_dbs = getComponentDatabases();

	if (cdb_component_dbs == NULL || cdb_component_dbs->total_entry_dbs <= 1)
	{
		elog(DEBUG1, "QD Mirroring: no mirror components found");
		disableQDMirroring();
		return;
	}

	/*
	 * We now have a list of our components. We if we have QDmirroring
	 * enabled, we should have more than one entry database
	 */
	if (cdb_component_dbs != NULL && cdb_component_dbs->total_entry_dbs > 1)
	{
		/* it should have maximum 2 dbs now */
		CdbComponentDatabaseInfo *entryDb = cdb_component_dbs->entry_db_info;
		CdbComponentDatabaseInfo *mirrorDb;

		/*
		 * If the first entry is the primary, then the second is the mirror
		 * if the first entry is the mirror, then the second is the primary.
		 *
		 * So we are assuming that we got back exactly two entry databases.
		 */
		if (entryDb->dbid == MppIdentity.dbid)
			mirrorDb = entryDb + 1;
		else
		{
			mirrorDb = entryDb;
			entryDb = entryDb + 1;
		}

		/* The management scripts "activate" the warm-QD by doing an
		 * update which points the QD-dbid at the warm-QD host. The
		 * currently active QD always has the same dbid, so the only
		 * way we can really *tell* that we're running in the
		 * fail-over mode, is if the two hostnames are the same
		 *
		 * Someday the "isprimary" fields will be updated as well.
		 */
		if (mirrorDb->isprimary || strcmp(mirrorDb->hostname, entryDb->hostname) == 0)
		{
			/* We are running as the mirror! When running as the
			 * mirror, we don't want to connect to ourselves! So we
			 * turn off mirroring after issuing a 'NOTICE' message.
			 */			
			elog(NOTICE, "QD Mirroring: mirror active, don't connect to self.");
			disableQDMirroring();
			return;
		}

		/* we are the primary connecting to the mirror */

		/* if it is set to invalid, do not connect */
		if (!mirrorDb->valid)
		{
			ereport(LOG, (errcode(ERRCODE_WARNING_MPP_INTERCONNECTION),
						  errmsg("QD Mirroring disabled for entry db "
								 "because it is not marked 'valid'.")));
			return;
		}

		cdb_mirrored_entry_db = malloc(sizeof(SegmentDatabaseDescriptor));
		memset(cdb_mirrored_entry_db, 0, sizeof(SegmentDatabaseDescriptor));
		cdbconn_initSegmentDescriptor(cdb_mirrored_entry_db, mirrorDb, NULL, NULL);

		/* connect to mirrored QD */
		if (!cdblink_connectQD(cdb_mirrored_entry_db))
		{
			//disableQDMirroring();

			ereport(WARNING, (errcode(ERRCODE_WARNING_MPP_INTERCONNECTION),
							  errmsg("QD Mirroring failed for entry db.  %s",
									 cdb_mirrored_entry_db->error_message.data)
						));
			cdb_mirrored_entry_db->errcode = 0;
			resetPQExpBuffer(&cdb_mirrored_entry_db->error_message);

			/* mark the db as down */
			/*
			  markSegDBStatus(cdb_mirrored_entry_db->segment_database_info->dbid, false);
			*/
		}
		else
		{
			FtsUpdateQDMirrorInfo(mirrorDb);
			/* verify connection */
			write_qd_sync("issyncmaster", NULL, 0);
		}
	}

	elog(DEBUG2, "QD mirror: connection status %s", PQstatus(cdb_mirrored_entry_db->conn) == CONNECTION_OK ? "good" : "bad");
}

static void
parseOffsetsFromMessage(char *buf, int *pfileoff, int *mfileoff)
{
	char		*start,
				*stop;
	int			p,
				m;

	Assert(buf != NULL);
	Assert(pfileoff != NULL);
	Assert(mfileoff != NULL);

	do {
		start = strstr(buf, "pdboff: ");
		if (start == NULL)
			break;
		stop = strstr(start, ",");
		p = atoi(start+strlen("pdboff: "));

		start = strstr(buf, "mdboff: ");
		if (start == NULL)
			break;
		stop = strstr(start, ",");
		m = atoi(start+strlen("mdboff: "));

		*pfileoff = p;
		*mfileoff = m;

		return;
	} while (0);

	/* error ! */
	ereport(ERROR, (errcode(ERRCODE_WARNING_MPP_INTERCONNECTION),
					errmsg("QD Mirroring: parse error on message returned from standby-QD (%s)", buf)));
}

/*
 * The bg-writer process needs to be able to build its communications
 * channel with the mirror-QD (if such exists), but has only the
 * cached values in shared memory.
 */
static void
buildQDMirrorFromShmem(void)
{
	uint16 port;
	CdbComponentDatabaseInfo mirrorDb;

	Assert(cdb_mirrored_entry_db == NULL);

	FtsGetQDMirrorInfo(&mirrorDb.hostname, &port);
	if (mirrorDb.hostname == NULL)
	{
		//elog(LOG, "buildQDMirrorFromShmem: no hostname for mirror");
		return;
	}

	mirrorDb.port = port;

	cdb_mirrored_entry_db = malloc(sizeof(SegmentDatabaseDescriptor));
	memset(cdb_mirrored_entry_db, 0, sizeof(SegmentDatabaseDescriptor));
	cdbconn_initSegmentDescriptor(cdb_mirrored_entry_db, &mirrorDb, NULL, NULL);

	/* connect to mirrored QD */
	if (!cdblink_connectQD(cdb_mirrored_entry_db))
	{
		ereport(WARNING, (errcode(ERRCODE_WARNING_MPP_INTERCONNECTION),
						  errmsg("QD Mirroring failed for entry db.  %s",
								 cdb_mirrored_entry_db->error_message.data)
					));
		cdb_mirrored_entry_db->errcode = 0;
		resetPQExpBuffer(&cdb_mirrored_entry_db->error_message);

		/* mark the db as down */
		/*
		  markSegDBStatus(cdb_mirrored_entry_db->segment_database_info->dbid, false);
		*/
	}
	else
	{
		/* verify connection */
		write_qd_sync("issyncmaster", NULL, 0);
	}

	return;
}

void
write_qd_sync(char *cmd, void *buf, int len)
{
	PGconn	   *conn;
	bool		failed = false;

	if (cdb_mirrored_entry_db == NULL)
	{
		buildQDMirrorFromShmem();
		/* did we successfully rebuild from shmem cache ? */
		if (cdb_mirrored_entry_db == NULL)
		{
			//disableQDMirroring();
			//elog(LOG, "QD mirror: no mirror");
			return;
		}
		/* we were able to rebuild the mirror entry from our shared-memory cache */
	}

	conn = cdb_mirrored_entry_db->conn;

	if (PQstatus(conn) != CONNECTION_OK)
	{
		elog(WARNING, "QD mirror: mirror connection failed");
		return;
	}

	if (PQsendQuery(conn, cmd) == 0)
	{
		failed = true;
	}
	else if (len > 0)
	{
		if (pqPutMsgStart(0, false, conn) < 0 ||
			pqPutnchar(buf, len, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			failed = true;
		else
			pqFlush(conn);
	}

	if (!failed)
	{
		/* get response */
		PGresult   *pRes = PQgetResult(conn);

		if (pRes != NULL)
		{
			ExecStatusType resultStatus = PQresultStatus(pRes);

			if (PGRES_COMMAND_OK != resultStatus &&
				PGRES_TUPLES_OK != resultStatus)
			{
				failed = true;
				if (!resync && strstr(PQresultErrorMessage(pRes), "resync") != NULL)
				{
					int			pfileoff=0,
								mfileoff=0;

					/* be careful going into an infinite loop */
					resync = true;

					/* resync tmlog and xlog */
					elog(DEBUG3, "write_qd_sync: result msg %s", PQresultErrorMessage(pRes));

					parseOffsetsFromMessage(PQresultErrorMessage(pRes), &pfileoff, &mfileoff);

					/* cleanup connection */
					do
					{
						PQclear(pRes);
					}
					while ((pRes = PQgetResult(conn)) != NULL);

					elog(DEBUG3, "write_qd_sync: pfileoff %d mfileoff %d", pfileoff, mfileoff);

					/* failed = true; */
					do
					{
						if (!resync_xlog_to_mirror())
						{
							elog(LOG, "write_qd_sync: xlog resync failed");
							break;
						}

						/* resync is successful */
						failed = false;
					} while (0);


					/* reset resync flag */
					resync = false;
				}
			}
			/* cleanup connection */
			do
			{
				PQclear(pRes);
			}
			while ((pRes = PQgetResult(conn)) != NULL);
		}

		if (PQstatus(conn) == CONNECTION_BAD)
			failed = true;
	}

	if (failed)
	{
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),		/* CDB TODO: better errcode */
				 errmsg("QD mirrored entry database write error: %s",
						PQerrorMessage(conn))));
		PQfinish(conn);
		cdb_mirrored_entry_db->conn = NULL;

		/*
		 * XXX: Uh, this disables mirroring for all subsequent sessions with
		 * this database! That can't possibly be the right behavior.
		 */
		disableQDMirroring();
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),		/* CDB TODO: better errcode */
				 errmsg("QD mirror: disabling QD mirror")));
	}
}

/* Connect to a QD-mirror as a client via libpq. */
static bool						/* returns true if connected */
cdblink_connectQD(SegmentDatabaseDescriptor * segdbDesc)
{
	CdbComponentDatabaseInfo *q = segdbDesc->segment_database_info;
	PQExpBufferData buffer;

	/*
	 * We use PQExpBufferData instead of StringInfoData
	 * because the former uses malloc, the latter palloc.
	 * We are in a thread, and we CANNOT use palloc since it's not
	 * thread safe.  We cannot call elog or ereport either for the
	 * same reason.
	 */
	initPQExpBuffer(&buffer);

	/*
	 * Build the connection string
	 */
	if (q->hostname == NULL)
		appendPQExpBufferStr(&buffer, "host='' ");
	else if (isdigit(q->hostname[0]))
		appendPQExpBuffer(&buffer, "hostaddr=%s ", q->hostname);
	else
		appendPQExpBuffer(&buffer, "host=%s ", q->hostname);

	appendPQExpBuffer(&buffer, "port=%u ", q->port);

	/*
	 * Call libpq to connect
	 */
	segdbDesc->conn = PQconnectdb(buffer.data);

	/*
	 * Check for connection failure.
	 */
	if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
	{
		if (!segdbDesc->errcode)
			segdbDesc->errcode = ERRCODE_MPP_INTERCONNECTION_ERROR;
		appendPQExpBuffer(&segdbDesc->error_message,
						  "QD unable to connect to %s with options %s: %s\n",
						  q->hostname,
						  buffer.data,
						  PQerrorMessage(segdbDesc->conn));

		elog(DEBUG2, segdbDesc->error_message.data);

		PQfinish(segdbDesc->conn);
		segdbDesc->conn = NULL;
	}
	/*
	 * Successfully connected.
	 */
	else
	{
		elog(DEBUG3, "Connected to mirror-QD %s with options %s\n",
			 q->hostname,
			 buffer.data);
	}

	free(buffer.data);
	return segdbDesc->conn != NULL;
}	/* cdblink_doConnect */

void *
getEntrySegDB(int *count)
{
	static CdbComponentDatabases *cdb_component_dbs = NULL;

	if (cdb_component_dbs == NULL)
		cdb_component_dbs = getCdbComponentDatabases();
	*count = cdb_component_dbs->total_entry_dbs;
	return cdb_component_dbs->entry_db_info;
}

void
disconnectMirrorQD(void)
{
	if (cdb_mirrored_entry_db == NULL || 
		PQstatus(cdb_mirrored_entry_db->conn) != CONNECTION_OK)
		return;

	write_qd_sync("close", NULL, 0);
	PQfinish(cdb_mirrored_entry_db->conn);

	return;
}
