/*-------------------------------------------------------------------------
 *
 * cdbfts.c
 *	  Provides fault tolerance service routines for mpp. 
 *
 * Copyright (c) 2003-2004, Greenplum
 *
 *-------------------------------------------------------------------------
 */

#include <assert.h>

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
#include "cdb/cdblink.h"
#include "cdb/cdbtm.h"
#include "commands/dbcommands.h"


bool         *ftsEnabled;
static LWLockId     ftsControlLock;
static int          *ftsSyncCount;
int          *ftsAction;
static bool         *ftsReadOnlyFlag;
static bool         *ftsQDMirroring;
static uint16       *ftsSegDBTransientState;
static int          *ftsSegDBTransientCount;

static PQExpBuffer  entryBuffer=NULL;
static PGconn       *entryConn=NULL;
static int          localFtsSyncCount=-1;

static void CdbShutdown(void);
static void initPQConnectionBuffer(CdbComponentDatabaseInfo *, PQExpBuffer, bool);
static void markSegDBTransientState(int, bool);
static void markSegDBPersistentState(int, bool);

#define GET_TRANSIENT_VALID_STATE(id)  (id >> 15)             /* first bit indicates valid column */
#define GET_TRANSIENT_ISPRIMARY_STATE(id) (id & 0x4000 >> 14) /* second bit indicates isPrimary column */
#define GET_TRANSIENT_SEGDB_ID(id) (id & 0x3FFF)              /* get segdb dbid */
#define SET_TRANSIENT_SEGDB_STATE_INVALID(id) (id)              /* set segdb valid and isPrimary flags */
#define SET_TRANSIENT_SEGDB_STATE_PRIMARY(id) (id | 0xC000)     /* set segdb valid and isPrimary flags */

#define incFtsSyncCount() (localFtsSyncCount = ++*ftsSyncCount)

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
        ftsQDMirroring = &shared->ftsQDMirroring;
        ftsSegDBTransientState = shared->ftsSegDBTransientState;
        ftsSegDBTransientCount = &shared->ftsSegDBTransientCount;
        
        //initialize
        shared->ftsSyncCount = 0;
        shared->ftsSegDBTransientCount = 0;
        shared->ftsReadOnlyFlag = mpp_set_read_only;
        shared->ftsQDMirroring = true;  /* set default to true, will be set to false if connection failed */
        if (Mpp_role == MPP_ROLE_DISPATCH)
        {
            shared->ftsAction = convertFaultActionValue(mpp_fault_action_string);
            elog(LOG, "FTS: Fault Tolerance Action is set to : %s", mpp_fault_action_string);
        }
        shared->ftsEnabled = (*ftsAction != FTS_NONE);
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

        assign_mpp_set_read_only(true, true, PGC_S_DEFAULT);
    }
}

/*
 * Re-Connect segDB
 */
bool FtsReConnectDB(void *failedDB)
{
    return reconnectSegDB(failedDB);
}

/*
 * Test-Connection
 */
bool FtsTestConnection(CdbComponentDatabaseInfo *failedDBInfo)
{
    PGconn *conn;
    bool ret = true;
    PQExpBuffer buffer = createPQExpBuffer();
    initPQConnectionBuffer(failedDBInfo, buffer, true);
    
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
    
    /* this code copied from verifyFtsSyncCount to avoid lock deadlock */
    if ( localFtsSyncCount == -1 )
    {
        localFtsSyncCount = *ftsSyncCount;
    }
    else if ( localFtsSyncCount != *ftsSyncCount )
    {
        localFtsSyncCount = *ftsSyncCount;
    }
    else
    {
        incFtsSyncCount();
        detectFailedConnections();
    }
    
    elog(LOG, "FTS: reconfiguration is in progress");
    disconnectAndDestroyAllGangs();
    allocateGang(GANGTYPE_PRIMARY_WRITER, getMPPSegmentCount(), -1);
    allocateGang(GANGTYPE_MIRROR_WRITER, getMPPSegmentCount(), -1);
    
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

void disableQDMirroring(void)
{
    elog(LOG, "FTS: QD mirroring disabled");
    *ftsQDMirroring = false;
}

bool isQDMirroringEnabled(void)
{
    return *ftsQDMirroring;
}

void FtsHandleNetFailure( SegmentDatabaseDescriptor **segDB, int numOfFailed )
{
    SegmentDatabaseDescriptor *failedDB;

    int i;
    for (i=0;i<numOfFailed;i++)
    {
        failedDB = segDB[i];

        if (FtsReConnectDB(failedDB))
            continue;
            
        if (!isFTSEnabled())
            continue;
            
        if (*ftsAction == FTS_SHUTDOWN)
        {
            elog(LOG, "MPP detects segment failures, system is shut down" );
            CdbShutdown();
            break;
        }
        else
        {
            FtsReConfigureMPP();
            elog(ERROR, "MPP detects segment failures, system is reconnected" );
            break;
        }
    }
}

/* call by createGang during creating connections
 * return true if error need to be thrown
 */
bool
FtsHandleGangConnectionFailure(SegmentDatabaseDescriptor *segdbDesc, int size)
{
    int i;
    bool reportError = false;
    if (!isFTSEnabled())
        return false;
        
    /* set MPP to be read only if necessary */
    CondSetMPPReadonly();

    elog(LOG, "FTS: reconfiguration is in progress");
    LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
    incFtsSyncCount();
    for ( i=0;i<size;i++ )
    {
        if ( (segdbDesc+i)->conn == NULL )
            markSegDBStatus((segdbDesc+i)->segment_database_info->dbid, false);
    }
    if (isGangExisted())
    {
        reportError = true;
        disconnectAndDestroyAllGangs();
    }
    
    /* rollback current DTM transaction */
    discardDtxTransaction();
    
    LWLockRelease(ftsControlLock);
    
    /* error out if this sets read only flag, at this stage the read only transaction
     * checking has passed, so error out, but do not error out if tm is in recoery
     */
    if ((*ftsReadOnlyFlag && !isTMInRecovery()) || reportError)
        return true;
    return false;
}

static void initPQConnectionBuffer(CdbComponentDatabaseInfo *q, PQExpBuffer buffer, bool utility)
{
    initPQExpBuffer(buffer);
    
    /*
     * Build the connection string
     */
    if ( utility )
        appendPQExpBuffer(buffer,
            "options='-c mpp_session_role=UTILITY' ");
    
    if ( q->hostname == NULL ) 
        appendPQExpBufferStr(buffer, "host='' " );
    else if ( isdigit(q->hostname[0]) )
        appendPQExpBuffer(buffer, "hostaddr=%s ", q->hostname );
    else 
        appendPQExpBuffer(buffer, "host=%s ", q->hostname );
                
    appendPQExpBuffer(buffer, "port=%u ", q->port);
    appendPQExpBuffer(buffer, "dbname=%s ", get_database_name(MyDatabaseId));
    appendPQExpBuffer(buffer, "user=%s ", q->mppsuperuser);

    if ( q->mppsuperpswd != NULL )
        appendPQExpBuffer(buffer, "password='%s' ", q->mppsuperpswd);
}

/* set seg db states */
void markSegDBStatus( int dbid, bool state )
{
    if (*ftsAction == FTS_READONLY)
        markSegDBTransientState(dbid, state);
    else
        markSegDBPersistentState(dbid, state);
}

/* set segdb state transiently */
void markSegDBTransientState( int dbid, bool state )
{
    /* this checking should be taken care by synchronization,
     * but check again for safety, may be removed in the future
     */
    if ( state )
    {
        /* we do not have this case right now */
        assert(false);
    }
    else
    {
        /* set transient state if state is false */
        int i, segindex;
        CdbComponentDatabaseInfo *segDb;
        CdbComponentDatabases *cdb_component_dbs = getComponentDatabases();        
        *(ftsSegDBTransientState+*ftsSegDBTransientCount) =
                                        SET_TRANSIENT_SEGDB_STATE_INVALID(dbid);
        
        if (*ftsSegDBTransientCount >= FTS_MAX_TRANSIENT_STATE)
            elog(ERROR, "FTS: the number of failed segdb exceeds the MPP limit");     
        
        (*ftsSegDBTransientCount)++;
        for (i=0;i<cdb_component_dbs->total_segment_dbs;i++)
        {
            segDb = cdb_component_dbs->segment_db_info+i;
            if (segDb->dbid == dbid)
                break;
        }
        
        /* assert on segdb not found */
        assert(i != cdb_component_dbs->total_segment_dbs);
        
        if (!segDb->isprimary)
        {
            /* we are done */
            return;
        }

        /* need to change isPrimary flag */
        segindex = segDb->segindex;
        for (i=0;i<cdb_component_dbs->total_segment_dbs;i++)
        {
            segDb = cdb_component_dbs->segment_db_info+i;
            if (segDb->segindex == segindex && segDb->dbid != dbid)
                break;
        }

        /* assert on segdb not found */
        assert(i != cdb_component_dbs->total_segment_dbs);
        
        if ( segDb->valid && !segDb->isprimary )
        {
            *(ftsSegDBTransientState+*ftsSegDBTransientCount) = 
                                        SET_TRANSIENT_SEGDB_STATE_PRIMARY(segDb->dbid);

            if (*ftsSegDBTransientCount >= FTS_MAX_TRANSIENT_STATE)
                elog(ERROR, "FTS: the number of failed segdb exceeds the MPP limit");     

            (*ftsSegDBTransientCount)++;
        }
    }
}

/* set segdb state permantally */
void markSegDBPersistentState( int dbid, bool state )
{
    int count;
    char cmd[100];
    PGresult *rs;
    CdbComponentDatabaseInfo *q = getEntrySegDB(&count);
    /* can only be maximum 2 dbs */
    if (count > 1)
        	if (q->dbid != MppIdentity.dbid)
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
        char *error_message = strdup(PQerrorMessage(entryConn));
         
        PQfinish(entryConn);
        entryConn = NULL;
        elog(PANIC, "FTS: setting segDB state failed, error connecting to entry db, error: %s", error_message );
    }
    
    sprintf(cmd, "select mpp.mpp_set_segdb_state(%d,%s)", dbid, state?"TRUE":"FALSE");
    
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

/*
 * Show hook routine for "mpp_role" option.
 *
 * See src/backend/util/misc/guc.c for option definition.
 */
const char *
show_mpp_set_read_only( void )
{
    return *ftsReadOnlyFlag?"TRUE":"FALSE";
}

void
FtsCondSetTxnReadOnly( bool *XactFlag)
{
    if (!isFTSEnabled())
        return;
    
    if (*ftsReadOnlyFlag && Mpp_role != MPP_ROLE_UTILITY)
        *XactFlag = true;
}

/*
 * Set Read only 
 */
bool
assign_mpp_set_read_only( bool newval, bool doit, GucSource source )
{

#if FALSE
    elog(DEBUG1, "mpp_set_read_only: %s, doit=%s",
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
        if (Mpp_role == MPP_ROLE_DISPATCH)
        {
            char cmdbuf[30];
            strcpy( cmdbuf, "set mpp_set_read_only=" );
            strcat( cmdbuf, newval?"true":"false");
            
            if ( !CdbDoCommandInt(cmdbuf, false, NULL) )
                elog(PANIC, "mpp_set_read_only failed on QE");
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
    else if (strcasecmp(action, "none") == 0)
        return FTS_NONE;
    else
    {
	    elog(LOG, "FTS: Invalid Fault Tolerance Action: %s", mpp_fault_action_string);
	    mpp_fault_action_string = "continue";
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
        elog(ERROR, "invalid string for mpp_set_read_only - %s", action );
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
    {
        elog(LOG, "FTS: detects re-configuration required");
        ret = false;
    }
        
    LWLockRelease(ftsControlLock);
    return ret;
}

/* apply transient state to segdb state */
CdbComponentDatabases *applyTransientState(CdbComponentDatabases *cdb_component_dbs)
{
    int i, j, dbid;
    CdbComponentDatabaseInfo *segDb;

    /* do not synchronzie here, it may cause sync deadlock */
    /*
    LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
    */
    
    for (i=0; i<*ftsSegDBTransientCount; i++)
    {
        dbid = GET_TRANSIENT_SEGDB_ID(*(ftsSegDBTransientState+i));
        
        for (j=0;j<cdb_component_dbs->total_segment_dbs; j++)
        {
            segDb = cdb_component_dbs->segment_db_info+j;
            if (segDb->dbid == dbid)
            {
                segDb->valid = GET_TRANSIENT_VALID_STATE(*(ftsSegDBTransientState+i));
                segDb->isprimary = GET_TRANSIENT_ISPRIMARY_STATE(*(ftsSegDBTransientState+i));
                break;
            }
        }
    }
    
    /*
    LWLockRelease(ftsControlLock);
    */
    return cdb_component_dbs;
}

/* re-test transient failed state */
void FtsEvaluateTransientState(void)
{
    int i, j, dbid;
    CdbComponentDatabases *cdb_component_dbs;
    CdbComponentDatabaseInfo *segDbInfo;
    
    if (*ftsSegDBTransientCount <= 0)
        return;

    cdb_component_dbs = getComponentDatabases();
    
    /* synchronzie fts */
    LWLockAcquire(ftsControlLock, LW_EXCLUSIVE);
    
    for (i=0; i<*ftsSegDBTransientCount; i++)
    {
        if (!GET_TRANSIENT_VALID_STATE(*(ftsSegDBTransientState+i)))
        {
            dbid = GET_TRANSIENT_SEGDB_ID(*(ftsSegDBTransientState+i));
            for (j=0;j<cdb_component_dbs->total_segment_dbs; j++)
            {
                segDbInfo = cdb_component_dbs->segment_db_info+j;
                if (segDbInfo->dbid == dbid)
                {
                    if (!FtsTestConnection(segDbInfo))
                    {
                        /* reconnect failed, keep current state */
                        LWLockRelease(ftsControlLock);
                        return;
                    }
                    break;
                }
            }
        }
    }

    /* retest ok, reset all states */
    *ftsSegDBTransientCount = 0;
    incFtsSyncCount();
    assign_mpp_set_read_only(false, true, PGC_S_DEFAULT);
    
    LWLockRelease(ftsControlLock);
    
}


