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
#include "cdb/cdbtmlog.h"


/*
 * Points to the SegmentDatabaseDescriptor of mirrored entry database
 */
static SegmentDatabaseDescriptor *cdb_mirrored_entry_db = NULL;
static bool resync = false;


/*
 * Called by backend/utils/init/postinit.c to setup stuffe
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
	 * If mpp_role is UTILITY, skip this call.
	 */


	if (Mpp_role == MPP_ROLE_UTILITY)
		return;

	getMppMpp();

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
 * when utiliy	no MPP catalog tables are read.  An Assert is
 * thrown if Mpp_role = utility.
 */
int
getMPPSegmentCount(void)
{
	if (Mpp_role == MPP_ROLE_UTILITY)
	{

		if (MppIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getMPPSegmentCount called when Mpp_role == utility. returning zero segments.");
			return 0;
		}

		elog(DEBUG1, "getMPPSegmentCount called when Mpp_role == utility, but is relying on mpp.mpp info");
		//log_backtrace();

	}

	if (Mpp_role == MPP_ROLE_EXECUTE)
	{
		if (MppIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getMPPSegmentCount called when Mpp_role == execute. returning zero segments.");
			return 0;
		}
		elog(DEBUG1, "getMPPSegmentCount called when Mpp_role == execute, but is relying on mpp.mpp info");
		//log_backtrace();
	}
	/* Assert( Mpp_role != MPP_ROLE_EXECUTE ); */

	if (MppIdentity.numsegments <= 0)
	{
		elog(LOG, "getMPPSegmentCount called BEFORE mpp.mpp record has been read in.");
		//log_backtrace();
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
     CdbComponentDatabases *cdb_component_dbs;
     if ( !isQDMirroringEnabled() )
        return;
     cdb_component_dbs = getComponentDatabases();
     if ( cdb_component_dbs != NULL && cdb_component_dbs->total_entry_dbs > 1)
     {
         /* it should have maximum 2 dbs now */
         CdbComponentDatabaseInfo *entryDb = cdb_component_dbs->entry_db_info;
         CdbComponentDatabaseInfo *mirrorDb;
         if (entryDb->dbid == MppIdentity.dbid)
            mirrorDb = entryDb + 1;
         else
         {
            mirrorDb = entryDb;
            entryDb = entryDb + 1;
         }

         if (!mirrorDb->isprimary)
         {
             char  *dbname = get_database_name(MyDatabaseId);

             /* if it is set to invalid, do not connect */
             if ( !mirrorDb->valid )
             {
                 ereport(WARNING,
                     (errcode(ERRCODE_WARNING),
                         errmsg("CDBLINK_SETUP--- mirrored entry database is set to invalid, skip connection"
                     )));
                 return;
             }

             /* create process descriptor */
             cdb_mirrored_entry_db = malloc(sizeof(SegmentDatabaseDescriptor));
             memset( cdb_mirrored_entry_db, 0, sizeof(SegmentDatabaseDescriptor));
             cdbconn_initSegmentDescriptor(cdb_mirrored_entry_db, mirrorDb);

             /* connect to mirrored QD */
             if ( !cdbconn_doConnect( cdb_mirrored_entry_db, dbname, NULL ) )
             {
                 disableQDMirroring();

                 ereport(LOG,
                         (errcode(ERRCODE_WARNING),     /* CDB TODO: better errcode */
                          errmsg("Unable to connect: %s",
                                 cdb_mirrored_entry_db->error_message.data)));
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
         }
     }
}

void
write_qd_sync(char *cmd, void *buf, int len)
{
	PGconn *conn;
	bool failed = false;
	
    if ( cdb_mirrored_entry_db == NULL || cdb_mirrored_entry_db->conn == NULL )
        return;

    elog(DEBUG3, "%s", cmd);
    conn = cdb_mirrored_entry_db->conn;


    if (0 == PQsendQuery(conn, cmd))
    {
        failed = true;
    }
    else if ( len > 0 )
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
        // get response
        PGresult *pRes = PQgetResult(conn);
        if ( pRes != NULL )
        {
            ExecStatusType resultStatus = PQresultStatus( pRes );
            if ( PGRES_COMMAND_OK != resultStatus &&
                 PGRES_TUPLES_OK != resultStatus )
            {
                failed = true;
                if (!resync && strstr(PQresultErrorMessage(pRes), "resync") != NULL)
                {
                    char dummy[100];
                    int pfileoff, mfileoff;

                    /* be careful going into an infinite loop */
                    resync = true;

                    /* resync tmlog and xlog */

                    sscanf(PQresultErrorMessage(pRes), "%s%s pdboff: %d, mdboff: %d",
                            dummy,dummy,&pfileoff,&mfileoff);

                    /* cleanup connection */
                    do
                    {
                        PQclear(pRes);
                    }
                    while ((pRes = PQgetResult(conn)) != NULL);

                    if (resync_xlog_to_mirror() &&
                        resync_tmlog_to_mirror(mfileoff, pfileoff))
                    {
                        /* resync is successful */
                        failed = false;
                    }

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

        if ( PQstatus( conn ) == CONNECTION_BAD )
            failed = true;
    }

    if (failed)
    {
        ereport(LOG,
                (errcode(ERRCODE_WARNING),      /* CDB TODO: better errcode */
                 errmsg("mirrored entry database write error: %s",
                        PQerrorMessage(conn))));
        PQfinish(conn);
        cdb_mirrored_entry_db->conn = NULL;
        disableQDMirroring();
    }
}

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
    if ( cdb_mirrored_entry_db == NULL || cdb_mirrored_entry_db->conn == NULL )
        return;

    write_qd_sync("close", NULL, 0);
    PQfinish(cdb_mirrored_entry_db->conn);
}
