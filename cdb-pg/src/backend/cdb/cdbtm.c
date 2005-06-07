/*-------------------------------------------------------------------------
 *
 * cdtm.c
 *	  Provides routines for performing distributed transaction
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *-------------------------------------------------------------------------
 */

#include <time.h>

#include "postgres.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbtmlog.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbvars.h"
#include "libpq-fe.h"


static LWLockId     shmControlLock;
static bool         *shmTmRecoverred;
static int          *shmGIDSeq;
static int          *shmNumGxacts;
static int          ControlLockCount=0;
int    *shmLatestLogFileNo;
int    *shmEarliestLogFileNo;

char dtm_dir[MAXPGPATH];

/*global transaction array */
static TMGXACT      **shmGxactArray, *currentGxact;

static bool duringRecovery = false;
static int max_tm_gxacts = 100;
static int checkpoint_interval = 5000;

// for semantic checking - debugging purpose
static int lastGxactState;

static const int transitionState[8][8]=
{
/*           IN-PROG  PREPARE 1P-CMT  2P-CMT ROLLBACK ABORT  CMTDONE RBDONE */
/* IN_PROG */{  1,      1,      1,      1,      1,      1,      0,      0},
/* PREPARE */{  0,      0,      0,      1,      1,      1,      0,      0},
/* 1P_CMT  */{  0,      0,      1,      0,      0,      1,      1,      0},
/* 2P_CMT  */{  0,      0,      0,      1,      0,      0,      1,      0},
/* ROLLBACK*/{  0,      0,      0,      0,      1,      1,      0,      1},
/* ABORT   */{  0,      0,      0,      0,      1,      1,      0,      0},
/* CMTDONE */{  0,      0,      0,      0,      0,      0,      0,      0},
/* RBDONE  */{  0,      0,      0,      0,      0,      0,      0,      0}
};

#define CHECK_DTX_STATE(curState, newState)  (transitionState[curState][newState])
#define SET_DTX_STATE(gxact, newState)       (gxact->state=newState)
//#define GET_SEQ_NO(shmGIDSeq)                    (shmGIDSeq & 0x00FFFFFF)
//#define GET_SEQ_NODE_NO(shmGIDSeq)               ((segno & 0xFF000000) >> 24)

/*
 *  starting a global transaction, called by user begin
 */
int
startDtxTransaction(void)
{
    if (Cdb_role != CDB_ROLE_DISPATCH)
        return DTX_OK;
    Assert( currentGxact == NULL || !currentGxact->autocommit );
    int ret = condStartDtxTransaction();
    currentGxact->autocommit=false;
    return ret;
}

/*
 * committing a global transaction
 */
int
commitDtxTransaction(bool onephase)
{
    if (Cdb_role != CDB_ROLE_DISPATCH || currentGxact == NULL)
        return DTX_OK;
    return condCommitDtxTransaction(onephase, true); 
}

/*
 * rollback a global transaction
 */
int
rollbackDtxTransaction(void)
{
    if (Cdb_role != CDB_ROLE_DISPATCH || currentGxact == NULL)
        return DTX_OK;
        
    // temporary ignore this call if it is in phase 2 commit, which can not
    // be rolled back.
    if (currentGxact->state == DTX_STATE_2P_COMMIT)
        return DTX_OK;
        
    return condRollbackDtxTransaction(true);
}

/*
 * starting a global transaction, call by internal command
 */
int
//startDtxTransaction( SegmentDatabaseDescriptor *dbs, int count )
condStartDtxTransaction(void)
{
    if ( currentGxact == NULL )
    {
        currentGxact = allocGxact();
        currentGxact->autocommit=true;
    }
    else
    {
        Assert( currentGxact->state == DTX_STATE_IN_PROGRESS );
        return DTX_OK;
    }
    
    // send additional begin transaction
    return execCommand();
    
}

int
condCommitDtxTransaction(bool onephase, bool force)
{
    if (currentGxact == NULL)
    {
        return DTX_ERR_PROTO;
    }
    if (!currentGxact->autocommit && !force)
        return DTX_OK;
    if (!onephase)
    {
        updateDtxState(DTX_STATE_PREPARED);
        
        // send prepare to all RM
        int ret = execCommand();
        if ( ret != DTX_OK )
            return ret;
        
        updateDtxState(DTX_STATE_2P_COMMIT);
        if ( (ret = execCommand()) != DTX_OK )
            return ret;
    }
    else
    {
        updateDtxState(DTX_STATE_1P_COMMIT);
        
        int ret = execCommand();
        if ( ret != DTX_OK )
            return ret;
    }
    
    updateDtxState(DTX_STATE_COMMIT_DONE);
    return DTX_OK;
}

int
condRollbackDtxTransaction(bool force)
{
    if (currentGxact == NULL)
    {
        return DTX_ERR_PROTO;
    }
    if (!currentGxact->autocommit && !force)
        return DTX_OK;
    
    updateDtxState(DTX_STATE_ROLLBACK);
        
    // send rollback to all RM
    int ret;
    PG_TRY();
    {
        ret=execCommand();
    }
    PG_CATCH();
    {
        ret = DTX_ERR_FAIL;
        if ( force )
            PG_RE_THROW();
    }
    PG_END_TRY();
    
    if ( !currentGxact->persistent || ret == DTX_OK )
    {
        // remove gxact either rollback is successful or xact is not prepared.
        updateDtxState(DTX_STATE_ROLLBACK_DONE);
    }
    return ret;
}

int
execCommand(void)
{
    char cmdbuf[50];
    
    switch (currentGxact->state)
    {
        case DTX_STATE_IN_PROGRESS:
            sprintf(cmdbuf, "%s'%s'", "BEGIN ", currentGxact->gid); 
            break;
        case DTX_STATE_PREPARED:
            sprintf(cmdbuf, "%s'%s'", "PREPARE ", currentGxact->gid); 
            break;
        case DTX_STATE_1P_COMMIT:
            sprintf(cmdbuf, "%s'%s'", "COMMIT ", currentGxact->gid); 
            break;
        case DTX_STATE_2P_COMMIT:
            sprintf(cmdbuf, "%s'%s'", "COMMIT PREPARED ", currentGxact->gid); 
            break;
        case DTX_STATE_ROLLBACK:
            sprintf(cmdbuf, "%s'%s'", "ROLLBACK ", currentGxact->gid); 
            break;
        case DTX_STATE_ABORT:
        case DTX_STATE_COMMIT_DONE:
        case DTX_STATE_ROLLBACK_DONE:
            // error
            Assert( currentGxact->state == 0 );
            break;        
    }
    

    if ( !CdbDoCommandInt(cmdbuf, DTS_SEGDB, true, NULL) )
    {
        // error
        switch (currentGxact->state)
        {
            case DTX_STATE_IN_PROGRESS:
            case DTX_STATE_PREPARED:
            case DTX_STATE_ROLLBACK:
            case DTX_STATE_1P_COMMIT:
                updateDtxState( DTX_STATE_ABORT );
                break;
            case DTX_STATE_2P_COMMIT:
                // keep this transaction for recovery 
                break;
            case DTX_STATE_ABORT:
            case DTX_STATE_COMMIT_DONE:
            case DTX_STATE_ROLLBACK_DONE:
                // error
                Assert( currentGxact->state == 0 );
                break;
        }

        return DTX_ERR_FAIL;
    }

    return DTX_OK;
}


int
startTMXactScan(void)
{
    getTmLock();
    return *shmNumGxacts;
}

TMGXACT *
getNextTMXact(int index)
{
    return shmGxactArray[index];
}

void
endTMXactScan(void)
{
    releaseTmLock();
}


void
initTM(void)
{
    if (!*shmTmRecoverred)
    {
        getTmLock();
        if (!*shmTmRecoverred)
        {
            PG_TRY();
            {
                recoverTM();
            }
            PG_CATCH();
            {
                releaseTmLock();
                PG_RE_THROW();
            }
            PG_END_TRY();
            *shmTmRecoverred = true;
        }
        releaseTmLock();
    }
    else
    {
        getTmLock();
        openTmLogFile(*shmLatestLogFileNo);
        releaseTmLock();
    }
}

int
tmShmemSize(void)
{
    if (Cdb_role != CDB_ROLE_DISPATCH)
        return 0;
    return MAXALIGN(sizeof(TmControlBlock) +
        max_tm_gxacts * (sizeof(TMGXACT*) + sizeof(TMGXACT)));
}


/*
 * tmShmemInit - should be called only once from postmaster and inherit by all
 * postgres processes
 */
void
tmShmemInit(void)
{
    if (Cdb_role != CDB_ROLE_DISPATCH)
        return;
    bool found;
    TmControlBlock *shared;

    shared = (TmControlBlock *)ShmemInitStruct("Transaction manager", tmShmemSize(), &found);
    if(!shared)
        elog(FATAL, "could not initialize transaction manager share memory");
   
    if (!IsUnderPostmaster)
            /* Initialize locks and shared memory area */
    {
        shared->ControlLock = LWLockAssign();
        shmControlLock = shared->ControlLock;
        shmTmRecoverred  = &shared->recoverred;
        shmGIDSeq = &shared->seqno;
        shmLatestLogFileNo = &shared->latestLogFileNo;
        shmEarliestLogFileNo = &shared->earliestLogFileNo;
        shmNumGxacts  = &shared->num_active_xacts;
        shmGxactArray = shared->gxact_array;
        
        // initialize gxact array
        int i=0;
        TMGXACT *gxact = (TMGXACT *)(shmGxactArray + max_tm_gxacts);
        for ( i=0;i<max_tm_gxacts;i++ )
            shmGxactArray[i] = gxact++;
        *shmEarliestLogFileNo = *shmLatestLogFileNo = -1;
    }

    /* Init directory path */
    snprintf(dtm_dir, MAXPGPATH, "%s/pg_dtm", DataDir);
}

bool
isInGlobalTransaction(void)
{
    return (currentGxact!=NULL);
}

void
initGxact( TMGXACT *gxact )
{
    generateGID(gxact->gid);
    gxact->state = DTX_STATE_IN_PROGRESS;
    gxact->autocommit = true;
    gxact->persistent = false;
    gxact->fileno = 0;
    gxact->offset = 0;
}

TMGXACT *
allocGxact(void)
{
    getTmLock();
    
    if ( *shmNumGxacts >= max_tm_gxacts )
    {
        releaseTmLock();
        ereport(ERROR, 
                    (errmsg("can not allocate global transaction")));
    }
    
    TMGXACT *gxact = shmGxactArray[(*shmNumGxacts)++];
    initGxact( gxact );
    releaseTmLock();

    return gxact;
}

void
restoreGxact(TMGXACT *gxact)
{
    if ( *shmNumGxacts >= max_tm_gxacts )
    {
        ereport(ERROR, 
                    (errmsg("can not allocate global transaction")));
    }
    
    currentGxact = shmGxactArray[(*shmNumGxacts)++];
    if ( gxact != NULL )
        memcpy( currentGxact, gxact, sizeof(TMGXACT));
}

void
redoTmLog(TmLogRecord *glog)
{
    int i=0;
    for (i=0;i<*shmNumGxacts;i++)
    {
        if (strcmp(glog->header.gid, shmGxactArray[i]->gid) == 0)
        {
            currentGxact = shmGxactArray[i];
            break;
        }
    }
    if ( i==*shmNumGxacts )
    {
        // transaction not found, this should be a prepare log
        Assert(glog->header.state == DTX_STATE_PREPARED ||
               glog->header.state == DTX_STATE_1P_COMMIT ||
               glog->header.state == DTX_STATE_ROLLBACK);
        restoreGxact( NULL );
        memcpy(currentGxact->gid, glog->header.gid, TMGIDSIZE);
        currentGxact->autocommit = true;
        currentGxact->persistent = true;
    }

    // use set, so log will not written again
    SET_DTX_STATE(currentGxact, glog->header.state);
    currentGxact->fileno = glog->header.thisFileNo;
    currentGxact->offset = glog->header.thisOffset;
    
    if (currentGxact->state == DTX_STATE_ROLLBACK_DONE ||
        currentGxact->state == DTX_STATE_COMMIT_DONE)
        releaseGxact();
}

void
releaseGxact(void)
{
    getTmLock();
    /* find slot of current transaction */
    int i=0;
    for(i=0; i < *shmNumGxacts; i++)
        if(shmGxactArray[i] == currentGxact)
            break;

    /*move this to the next available slot */
    (*shmNumGxacts)--;
    if (i!=*shmNumGxacts)
    {
        shmGxactArray[i] = shmGxactArray[*shmNumGxacts];
        shmGxactArray[*shmNumGxacts] = currentGxact;
    }
    currentGxact = NULL;
    releaseTmLock();
}


/*
 * update transaction state
 */
int
updateDtxState(DtxState state) 
{
    TMGXACT *gxact = currentGxact;
    Assert( gxact != NULL );

    ereport(DEBUG1, (errmsg("updateDtxState: %s to state: %d", gxact->gid, state)));

    switch (state)
    {
        case    DTX_STATE_PREPARED:
        case    DTX_STATE_1P_COMMIT:
        case    DTX_STATE_2P_COMMIT:
        case    DTX_STATE_ROLLBACK:
            /* if checking ok, set the state */
            if ( !CHECK_DTX_STATE(gxact->state, state ) )
            {
                // internal error, shouldn't happen
                Assert( 0 == 1 );
                return DTX_ERR_PROTO;
            }
            break;
        case    DTX_STATE_ABORT:
        case    DTX_STATE_COMMIT_DONE:
        case    DTX_STATE_ROLLBACK_DONE:
        default:
            break;
    }
    
    // write tm log
    TmLogRecord record={{0,0,"",TM_LOG_XACT,0,0},{}};
    strcpy(record.header.gid, gxact->gid);
    record.header.state = state;
    
    // synchronizing log file write
    getTmLock();
    writeTmLogRecord(&record);

    // set memory state
    SET_DTX_STATE(gxact, state);
    currentGxact->fileno = record.header.thisFileNo;
    currentGxact->offset = record.header.thisOffset;
    
    if (state == DTX_STATE_COMMIT_DONE || state == DTX_STATE_ROLLBACK_DONE)
        releaseGxact();

    if (state == DTX_STATE_PREPARED)
        gxact->persistent = true;
            
    // checkpoint
    if ( !duringRecovery && ((*shmGIDSeq) % checkpoint_interval) == 0 )
    {
        checkPointTm();
        (*shmGIDSeq)++;
    }
    else
        flushTmLogFile();
        
    releaseTmLock();
    
    lastGxactState = state;

    return DTX_OK;
}

void
generateGID(char *gid)
{
    time_t t = time(NULL);
    if (t == (time_t)-1)
    {
        ereport(LOG, 
            (errmsg("can not generate global id properly")));
    }
    
    // tm lock acquired by caller
    sprintf( gid, "%ld-%.10d", t, (*shmGIDSeq)++);
}

void
shutdownTM(void)
{
    if (*shmLatestLogFileNo == -1 )
        return;
    getTmLock();
    if (*shmLatestLogFileNo != -1 )
    {
        if ( *shmNumGxacts != 0 )
        {
            releaseTmLock();
            return;
        }
        checkPointTm();
    }
    releaseTmLock();
}

void
recoverTM(void)
{
    duringRecovery = true;
    
    // phase 1 recovery
    // read and rollforward tm log
    recoverTmLog();
    
    // phase 2 recovery
    if ( !phase2Recovery() )
        return;
    
    // check any un-resolved transactions in TM
    if ( *shmNumGxacts != 0 )
    {
        // still have in-doubt transactions, save it to log
        int i=0;
        for ( i=0;i<*shmNumGxacts;i++ )
            ereport(LOG, 
                    (errmsg("global xact %s pending on TM", shmGxactArray[i]->gid)));
    }

    // check again to see any un-resolved transactions in QE
    PGresult **pgresultSets;
    if ( recoverRMs( &pgresultSets ) )
    {
        // still have in-doubt transactions in QE, save it to log
        dumpRMxacts( pgresultSets );
    }
    
    cleanRMResultSets( pgresultSets );
    
    // finish recovery, do checkpoint
    checkPointTm();
    duringRecovery = false;
}

bool
phase2Recovery(void)
{
    // get all in-doubt transactions from QEs
    PGresult **pgresultSets;
    bool needRecovery = recoverRMs( &pgresultSets );
    
    if ( *shmNumGxacts == 0 && !needRecovery )
    {
        checkPointTm();
        duringRecovery = false;
        cleanRMResultSets( pgresultSets );
        currentGxact = NULL;
        return false;
    }
    
    int i=0;
    for ( i=0;i<*shmNumGxacts;i++ )
    {
        int ret=DTX_OK;
        currentGxact = shmGxactArray[i];
        
        switch (currentGxact->state)
        {
            case DTX_STATE_IN_PROGRESS:
            case DTX_STATE_PREPARED:
            case DTX_STATE_1P_COMMIT:
            case DTX_STATE_ROLLBACK:
            case DTX_STATE_ABORT:
                if ( checkRM( pgresultSets ) )
                    ret = condRollbackDtxTransaction(false);
                else
                {
                    updateDtxState(DTX_STATE_ROLLBACK_DONE);
                }
                break;
                
            case DTX_STATE_2P_COMMIT:
                if ( checkRM( pgresultSets ) )
                    ret = handleCommittedDtx();
                else
                {
                    updateDtxState(DTX_STATE_COMMIT_DONE);
                }
                break;

            case DTX_STATE_COMMIT_DONE:
            case DTX_STATE_ROLLBACK_DONE:
            default:
                ereport(LOG, 
                    (errmsg("invalid state during recovery gid: %s, state: %d", currentGxact->gid, currentGxact->state)));
                releaseGxact();
                break;
        }
        
        if ( ret == DTX_OK )
            // transaction is removed, slot is shift one left, so adjust i as well
            i--;
    }
    
    cleanRMResultSets( pgresultSets );
    currentGxact = NULL;
    
    return true;
}

void
cleanRMResultSets( PGresult **pgresultSets )
{
    // TODO: need to handle multiple dbs in a segment case
    //int tolaldbs = CdbClusterDefinition->segment_count * CdbClusterDefinition->db_count;
    int totaldbs = CdbClusterDefinition->segment_count;
    int i=0;
    for ( i=0;i<totaldbs;i++ )
    {
        PGresult *rs = pgresultSets[i];
        PQclear(rs);
    }
    free(pgresultSets);
}

bool
dumpRMxacts( PGresult **pgresultSets )
{
    int totaldbs = CdbClusterDefinition->segment_count;
    
    int i=0, j=0;
    for ( i=0;i<totaldbs;i++ )
    {
        PGresult *rs = pgresultSets[i];
        int rows = PQntuples(rs);
        {
            for (j=0;j<rows;j++)
            {
                ereport(ERROR, 
                    (errmsg("global xact %s pending on QE%d and not found in TM",
                        PQgetvalue(rs,j,0),i)));
            }
        }
    }
    return false;
}

/*
 * checkRM - check recovery is required on RM
 */
bool
checkRM( PGresult **pgresultSets )
{
    int totaldbs = CdbClusterDefinition->segment_count;
    
    int i=0, j=0;
    for ( i=0;i<totaldbs;i++ )
    {
        PGresult *rs = pgresultSets[i];
        int rows = PQntuples(rs);
        {
            for (j=0;j<rows;j++)
            {
                if ( strcmp(currentGxact->gid, PQgetvalue(rs,j,0)) == 0 )
                    return true;
            }
        }
    }
    return false;
}

/*
 * recoverRMs - collect all prepared transactions from RM
 */
bool
recoverRMs(PGresult ***pgresultSets)
{
    char cmdbuf[] = "select gid from pg_prepared_xacts";
    
    if ( !CdbDoCommandInt(cmdbuf, DTS_SEGDB, true, pgresultSets) )
    {
        return true;
    }
    
    int totaldbs = CdbClusterDefinition->segment_count;
    
    int i=0;
    for ( i=0;i<totaldbs;i++ )
    {
        PGresult *rs = (*pgresultSets)[i];
        if (PQntuples(rs) != 0)
            return true;
    }
    
    return false;
}

bool
handleCommittedDtx(void)
{
    // send second phase commit to QE
    int ret;
    PG_TRY();
    {
        ret=execCommand();
    }
    PG_CATCH();
    {
        ret = DTX_ERR_FAIL;
    }
    PG_END_TRY();
    
    if ( (ret = execCommand()) != DTX_OK )
        return ret;
        
    updateDtxState(DTX_STATE_COMMIT_DONE);
    
    return DTX_OK;
}

void
getTmLock(void)
{
    if ( ControlLockCount++ == 0 )
        LWLockAcquire(shmControlLock, LW_EXCLUSIVE);
}

void
releaseTmLock(void)
{
    if ( --ControlLockCount == 0 )
        LWLockRelease(shmControlLock);
}

/*
 * Semantic Checking - all this from QD to make sure both QD and QE transaction
 * are all commit or all rollback
 * --   For debugging purpose
 */
void
semanticCheckingOnQE(bool commit)
{
    if ( commit )
        Assert(lastGxactState == DTX_STATE_2P_COMMIT ||
                lastGxactState == DTX_STATE_COMMIT_DONE);
    else
        Assert(lastGxactState == DTX_STATE_ROLLBACK ||
                lastGxactState == DTX_STATE_ROLLBACK_DONE);
}
