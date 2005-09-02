/*-------------------------------------------------------------------------
 *
 * cdblink.c
 *
 *	  Setting up libpq connections to qRxecs from Dispatcher
 *
 *
 * ATTRIBUTION
 *
 *	   Concept and initial logic based on contrib/dblink by:
 *
 *	   Joe Conway <mail@joeconway.com>
 *	   And contributors:
 *	   Darko Prenosil <Darko.Prenosil@finteh.hr>
 *	   Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 *	   Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *	   ALL RIGHTS RESERVED;
 *
 *	   Permission to use, copy, modify, and distribute this software and its
 *	   documentation for any purpose, without fee, and without a written agreement
 *	   is hereby granted, provided that the above copyright notice and this
 *	   paragraph and the following two paragraphs appear in all copies.
 *
 *	   IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 *	   DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 *	   LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 *	   DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 *	   POSSIBILITY OF SUCH DAMAGE.
 *
 *	   THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 *	   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 *	   AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 *	   ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 *	   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 *
 */
#include "postgres.h"

#include <ctype.h>
#include <pthread.h>

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

#include "cdb/cdbcat.h"
#include "cdb/cdbconn.h"
#include "cdb/cdblink.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbfts.h"


static void CdbDoConnect(SegmentDatabaseDescriptor *);

/*     
 * Points to the SegmentDatabaseDescriptor of mirrored entry database
 */
static SegmentDatabaseDescriptor *cdb_mirrored_entry_db = NULL;


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
	 * If cdb_role is UTILITY, skip this call.
	 */


	if (Cdb_role == CDB_ROLE_UTILITY)
		return;

	getCdbCdb();

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
 * N.B.  Cdb_role must be either dispatch or execute, since
 * when utiliy	no cdb catalog tables are read.  An Assert is
 * thrown if Cdb_role = utility.
 */
int
getCdbSegmentCount(void)
{
	if (Cdb_role == CDB_ROLE_UTILITY)
	{

		if (CdbIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getCdbSegmentCount called when Cdb_role == utility. returning zero segments.");
			return 0;
		}

		elog(DEBUG1, "getCdbSegmentCount called when Cdb_role == utility, but is relying on cdb.cdb info");
		//log_backtrace();

	}

	if (Cdb_role == CDB_ROLE_EXECUTE)
	{
		if (CdbIdentity.numsegments <= 0)
		{
			elog(DEBUG5, "getCdbSegmentCount called when Cdb_role == execute. returning zero segments.");
			return 0;
		}
		elog(DEBUG1, "getCdbSegmentCount called when Cdb_role == execute, but is relying on cdb.cdb info");
		//log_backtrace();
	}
	/* Assert( Cdb_role != CDB_ROLE_EXECUTE ); */

	if (CdbIdentity.numsegments <= 0)
	{
		elog(DEBUG1, "getCdbSegmentCount called BEFORE cdb.cdb record has been read in.");
		//log_backtrace();
	}

	/* Assert( Cdb_role != CDB_ROLE_UTILITY ); */

	return CdbIdentity.numsegments;
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
         if (entryDb->dbid == CdbIdentity.dbid)
            mirrorDb = entryDb + 1;
         else
         {
            mirrorDb = entryDb;
            entryDb = entryDb + 1;
         }
         
         if (!mirrorDb->isprimary)
         {
             /* if it is set to invalid, do not connect */
             if ( !mirrorDb->valid )
             {
                 ereport(WARNING,
                     (errcode(ERRCODE_WARNING),
                         errmsg("CDBLINK_SETUP--- mirrored entry database is set to invalid, skip connection"
                     )));
                 return;
             }
             
             /* connect to mirrored QD */
             cdb_mirrored_entry_db = malloc(sizeof(SegmentDatabaseDescriptor));
             memset( cdb_mirrored_entry_db, 0, sizeof(SegmentDatabaseDescriptor));
             cdb_mirrored_entry_db->segment_database_info = mirrorDb;
             CdbDoConnect( cdb_mirrored_entry_db );
             if ( cdb_mirrored_entry_db->conn == NULL )
             {
                 char* pszErrorMessage = cdb_mirrored_entry_db->error_message;
                 Assert( pszErrorMessage != NULL );
                 MemoryContext oldContext = MemoryContextSwitchTo(TopMemoryContext);
                 cdb_mirrored_entry_db->error_message = pstrdup(pszErrorMessage);
                 MemoryContextSwitchTo(oldContext);
                 free( pszErrorMessage );
                 
                 disableQDMirroring();
 
                 ereport(WARNING,
                     (errcode(ERRCODE_WARNING),
                         errmsg("CDBLINK_SETUP--- Could not establish connection with mirrored entry database "
                             "segindex %d isprimary %c", 
                             mirrorDb->segindex, (mirrorDb->isprimary ? 't' : 'f')),
                         errdetail("%s", cdb_mirrored_entry_db->error_message)));

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

static void
CdbDoConnect( SegmentDatabaseDescriptor *segdbDesc )
{
    CdbComponentDatabaseInfo *q = segdbDesc->segment_database_info;
    /*
     * We use PQExpBufferData instead of StringInfoData
     * because the former uses malloc, the latter palloc.
     * We are in a thread, and we CANNOT use palloc since it's not
     * thread safe.  We cannot call elog or ereport either for the
     * same reason.
     */
    PQExpBufferData buffer;
    initPQExpBuffer(&buffer);
     
    /*
     * Build the connection string
     */    
    if ( q->hostname == NULL ) 
        appendPQExpBufferStr(&buffer, "host='' " );
    else if ( isdigit(q->hostname[0]) )
        appendPQExpBuffer(&buffer, "hostaddr=%s ", q->hostname );
    else 
        appendPQExpBuffer(&buffer, "host=%s ", q->hostname );
                 
    appendPQExpBuffer(&buffer, "port=%u ", q->port);
    appendPQExpBuffer(&buffer, "dbname=%s ", get_database_name(MyDatabaseId));
    appendPQExpBuffer(&buffer, "user=%s ", q->cdbsuperuser);

    if ( q->cdbsuperpswd != NULL )
        appendPQExpBuffer(&buffer, "password='%s' ", q->cdbsuperpswd);

    /*
     * Call libpq to connect
     */                 
    PGconn *conn = PQconnectdb(buffer.data);

    if ( PQstatus(conn) == CONNECTION_BAD )
    {
        /*
         * When we get an error, we strdup it here.  When the main thread
         * checks for errors, it makes a palloc copy of this, and frees
         * this.
         */                 
        segdbDesc->error_message = strdup(PQerrorMessage(conn));
        PQfinish(conn);
        conn = NULL;
    }

    segdbDesc->conn = conn;
    free(buffer.data);
}

void
write_qd_sync(char *cmd, void *buf, int len)
{
    elog(DEBUG3, "%s", cmd);
    if ( cdb_mirrored_entry_db == NULL || cdb_mirrored_entry_db->conn == NULL )
        return;
    PGconn *conn = cdb_mirrored_entry_db->conn;
    bool failed = false;

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
            cdb_mirrored_entry_db->resultStatus = PQresultStatus( pRes );
            if ( PGRES_COMMAND_OK != cdb_mirrored_entry_db->resultStatus &&
                PGRES_TUPLES_OK != cdb_mirrored_entry_db->resultStatus )
                failed = true;
        }
        
        if ( PQstatus( conn ) == CONNECTION_BAD )
            failed = true;
    }
    
    if (failed)
    {
        char *error_message = strdup(PQerrorMessage(conn));
        ereport(WARNING,
             (errcode(ERRCODE_WARNING),
                 errmsg("mirrored entry database write error "),
                 errdetail("%s", error_message)));
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
