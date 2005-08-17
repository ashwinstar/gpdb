/*-------------------------------------------------------------------------
 *
 * cdbfts.c
 *	  Provides fault tolerance service routines for CDB. 
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbdisp.h"
#include "access/xact.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"
#include "cdb/cdblink.h"
#include "cdb/cdbtm.h"
#include "commands/dbcommands.h"


static bool         *ftsEnabled;
static LWLockId     ftsControlLock;
int                 *ftsSyncCount;
static int          *ftsAction;
bool                *ftsReadOnlyFlag;

static int          curAction;
static PQExpBuffer  entryBuffer=NULL;
static PGconn       *entryConn=NULL;
static int          localFtsSyncCount=-1;


static bool FtsDoAction(int, SegmentDatabaseDescriptor *);
static int FtsNextAction(void);
static void CdbShutdown(void);
static void initPQConnectionBuffer(CdbComponentDatabaseInfo *, PQExpBuffer, bool);
static void incFtsSyncCount(void);


/* get fts share memory size */
int
FtsShmemSize(void)
{
    return MAXALIGN(sizeof(FtsControlBlock));
}


void FtsShmemInit(void)
{
    bool found;
    FtsControlBlock *shared;

    shared = (FtsControlBlock *)ShmemInitStruct("Fault Tolerance manager", FtsShmemSize(), &found);
    if(!shared)
        elog(FATAL, "FTS: could not initialize fault tolerance manager share memory");

    if (!IsUnderPostmaster)
            /* Initialize locks and shared memory area */
    {
        ftsEnabled = &shared->ftsEnabled;
        shared->ControlLock = LWLockAssign();
        ftsControlLock = shared->ControlLock;
        ftsSyncCount  = &shared->ftsSyncCount;
        ftsAction = &shared->ftsAction;
        ftsReadOnlyFlag = &shared->ftsReadOnlyFlag;
        
        //initialize
        shared->ftsSyncCount = 0;
        shared->ftsReadOnlyFlag = cdb_set_read_only;
        shared->ftsEnabled = false;
        if (Cdb_role == CDB_ROLE_DISPATCH)
        {
            shared->ftsAction = convertFaultActionValue(cdb_fault_action_string);
            elog(LOG, "FTS: Fault Tolerance Action is set to : %s", cdb_fault_action_string);
        }
    }
}

/*
 * do action
 */
static bool FtsDoAction(int p_action, SegmentDatabaseDescriptor *failedDB)
{
    bool ret=false;
    switch ( p_action )
    {
        case FTS_RECONNECT:
            ret = FtsReConnectDB(failedDB);
            break;
        case FTS_READONLY:
            CondSetMPPReadonly();
        case FTS_CONTINUE:
            FtsReConfigureMPP();
            elog(ERROR, "MPP detects segment failures, system restarted" );
            break;
        case FTS_SHUTDOWN:
            CdbShutdown();
            break;
    }
    
    return ret;
}

/*
 * Get Next Action
 */
static int FtsNextAction(void)
{
    switch ( curAction )
    {
        case FTS_RECONNECT:
            return curAction++;
        case FTS_CONTINUE:
        case FTS_READONLY:
        case FTS_SHUTDOWN:
            curAction = -1;
            return *ftsAction;
        default:
            /* error case */
            return -1;
    }
}


/*
 * Set System Read Only
 */
void CondSetMPPReadonly(void)
{
    if (*ftsAction == FTS_READONLY)
    {
        elog(LOG, "FTS: MPP system is set read only");

        assign_cdb_set_read_only(true, true, PGC_S_DEFAULT);
    }
}

/*
 * Re-Connect segDB
 */
bool FtsReConnectDB(void *failedDB)
{
    bool dbconnstatus = reconnectSegDB(failedDB);
    return dbconnstatus;
}

/*
 * Test-Connection
 */
bool FtsTestConnection(SegmentDatabaseDescriptor *failedDB)
{
    PGconn *conn;
    bool ret = true;
    PQExpBuffer buffer = createPQExpBuffer();
    initPQConnectionBuffer(failedDB->segment_database_info, buffer, true);
    
    conn = PQconnectdb(buffer->data);

    if ( PQstatus(conn) == CONNECTION_BAD )
        ret = false;
    
    PQfinish(conn);
    return ret;
}

/*
 * Re-Configure the system
 */
bool FtsReConfigureMPP(void)
{
    /* synchronzie fts */
    LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);

    if (!verifyFtsSyncCount())
    {
        localFtsSyncCount = *ftsSyncCount;
    }
    else
    {
        incFtsSyncCount();
        detectFailedConnections();
    }
    
    disconnectAndDestroyAllGangs();
    allocateGang(GANGTYPE_PRIMARY_WRITER, getCdbSegmentCount(), -1);
    allocateGang(GANGTYPE_MIRROR_WRITER, getCdbSegmentCount(), -1);
    
    /* rollback current DTM transaction */
    discardDtxTransaction();
    
    LWLockRelease(ftsControlLock);
    
    return true;
}

/*
 * Shutdown the system
 */
static void CdbShutdown(void)
{
    elog(PANIC, "FTS: MPP system is shuted down");
}

/*
static int getFtsSyncCount(void)
{
    return *ftsSyncCount;
}
*/

/*
bool isFtsEnabled(void)
{
    return *ftsEnabled;
}
*/

void handleNetFailure( SegmentDatabaseDescriptor **segDB, int numOfFailed )
{
    int i;
    SegmentDatabaseDescriptor *failedDB;
    if (!ftsEnabled)
        return;
    
    for (i=0;i<numOfFailed;i++)
    {
        failedDB = segDB[i];

        curAction = 0;
        int nextAction;
        while ((nextAction=FtsNextAction()) != -1)
        {
            if ( !FtsDoAction(nextAction, failedDB) )
            {
                if (nextAction > FTS_RECONNECT)
                    return;
            }
            else
                break;
        }
    }
}

static void initPQConnectionBuffer(CdbComponentDatabaseInfo *q, PQExpBuffer buffer, bool utility)
{
    initPQExpBuffer(buffer);
    
    /*
     * Build the connection string
     */
    if ( utility )
        appendPQExpBuffer(buffer,
            "options='-c cdb_session_role=UTILITY' ");
    
    if ( q->hostname == NULL ) 
        appendPQExpBufferStr(buffer, "host='' " );
    else if ( isdigit(q->hostname[0]) )
        appendPQExpBuffer(buffer, "hostaddr=%s ", q->hostname );
    else 
        appendPQExpBuffer(buffer, "host=%s ", q->hostname );
                
    appendPQExpBuffer(buffer, "port=%u ", q->port);
    appendPQExpBuffer(buffer, "dbname=%s ", get_database_name(MyDatabaseId));
    appendPQExpBuffer(buffer, "user=%s ", q->cdbsuperuser);

    if ( q->cdbsuperpswd != NULL )
        appendPQExpBuffer(buffer, "password='%s' ", q->cdbsuperpswd);
}

/* set seg db states */
void markSegDBStatus( int dbid, bool state )
{
    int count;
    char cmd[100];
    PGresult *rs;
    CdbComponentDatabaseInfo *q = getEntrySegDB(&count);
    /* can only be maximum 2 dbs */
    if (count > 1)
        	if (!(q->dbid != CdbIdentity.dbid))
			q = q+1;
     
    /*
     * We use PQExpBufferData instead of StringInfoData
     * because the former uses malloc, the latter palloc.
     * We are in a thread, and we CANNOT use palloc since it's not
     * thread safe.  We cannot call elog or ereport either for the
     * same reason.
     */
    if (entryBuffer == NULL)
    {
        entryBuffer = createPQExpBuffer();
        
        /* initialize libpq connection buffer, we only need to initialize it once */
        initPQConnectionBuffer(q, entryBuffer, true);
    }
    
    /*
     * Call libpq to connect
     */            
    if (entryConn == NULL) 
        entryConn = PQconnectdb(entryBuffer->data);

    if ( PQstatus(entryConn) == CONNECTION_BAD )
    {
        /*
         * When we get an error, we strdup it here.  When the main thread
         * checks for errors, it makes a palloc copy of this, and frees
         * this.
         */                 
        PQfinish(entryConn);
        entryConn = NULL;
        elog(PANIC, "FTS: setting segDB state failed, error connecting to entry db" );
    }
    
    sprintf(cmd, "select cdb.cdb_set_segdb_state(%d,%s)", dbid, state?"TRUE":"FALSE");
    
    rs = PQexec(entryConn, cmd);
    
    if (PQresultStatus(rs) != PGRES_TUPLES_OK)
    {
        PQclear(rs);
        PQfinish(entryConn);
        entryConn = NULL;
        elog(PANIC, "FTS: setting segDB state failed" );
    }
    
    elog(LOG, "FTS: mark segDB %d as %s", dbid, state?"valid":"invalid" );
    PQclear(rs);
    PQfinish(entryConn);
    entryConn = NULL;
}

/* call by createGang during creating connections */
void
markGangStatus(SegmentDatabaseDescriptor *segdbDesc, int size)
{
    int i;
    
    LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
    incFtsSyncCount();
    for ( i=0;i<size;i++ )
    {
        if ( (segdbDesc+i)->conn == NULL )
            markSegDBStatus((segdbDesc+i)->segment_database_info->dbid, false);
    }
    incFtsSyncCount();
    LWLockRelease(ftsControlLock);
}

/*
 * Show hook routine for "cdb_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_cdb_set_read_only( void )
{
    return *ftsReadOnlyFlag?"TRUE":"FALSE";
}
/*
 * Assign hook routine for "cdb_role" option.  This variablle has context
 * PGC_SUSET so that is can only be set by a superuser via the SET command.
 * (It can also be set using an option on postmaster start, but this isn't
 * interesting beccause the derived global CdbRole is always set (along with
 * CdbSessionRole) on backend startup for a new connection.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
bool
assign_cdb_set_read_only( bool newval, bool doit, GucSource source )
{

#if FALSE
    elog(DEBUG1, "cdb_set_read_only: %s, doit=%s",
            newval, ( doit ? "true" : "false" ));
#endif

    if ( ftsReadOnlyFlag == NULL || newval == *ftsReadOnlyFlag)
        return true;

    if ( doit )
    {
        *ftsReadOnlyFlag = newval;
        
        // set current transaction read only
        XactReadOnly = newval;
        
  
        /*      
        if (Cdb_role == CDB_ROLE_DISPATCH)
        {
            char cmdbuf[30];
            strcpy( cmdbuf, "set cdb_set_read_only=" );
            strcat( cmdbuf, newval?"true":"false");
            
            if ( !CdbDoCommandInt(cmdbuf, DTS_SEGDB, false, NULL) )
                elog(PANIC, "cdb_set_read_only failed on QE");
        }
        */
    }

    return true;
}

int convertFaultActionValue(char *action)
{
    if (strcasecmp(action, "readonly") == 0)
	    return FTS_READONLY;
    else if (strcasecmp(action, "shutdown") == 0)
    	    return FTS_SHUTDOWN;
    else if (strcasecmp(action, "continue") == 0)
	    return FTS_CONTINUE;
    else
    {
	    elog(LOG, "FTS: Invalid Fault Tolerance Action: %s", cdb_fault_action_string);
	    cdb_fault_action_string = "continue";
	    return FTS_CONTINUE;
    }
}

/*
bool convertReadOnlyValue(char *action)
{
    if (strcasecmp(action, "true")==0)
        return true;
    else if (strcasecmp(action, "false")==0)
        return false;
    else
        elog(ERROR, "invalid string for cdb_set_read_only - %s", action );
}
*/

bool verifyFtsSyncCount(void)
{
    bool ret=true;
    /* synchronzie fts */
    LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);

    if ( localFtsSyncCount == -1 )
        localFtsSyncCount = *ftsSyncCount;
    else if ( localFtsSyncCount != *ftsSyncCount )
        ret = false;
        
    LWLockRelease(ftsControlLock);
    return ret;
}

static void incFtsSyncCount(void)
{
    localFtsSyncCount = ++*ftsSyncCount; 
}



