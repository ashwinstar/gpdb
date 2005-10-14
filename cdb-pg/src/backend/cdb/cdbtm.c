/*-------------------------------------------------------------------------
 *
 * cdtm.c
 *	  Provides routines for performing distributed transaction
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include "cdb/cdbtm.h"
#include "cdb/cdbtmlog.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbvars.h"
#include "libpq-fe.h"
#include "access/xact.h"
#include "cdb/cdbfts.h"
#include "lib/stringinfo.h"


static LWLockId     shmControlLock;
static bool         *shmTmRecoverred;
static int          *shmGIDSeq;
static int          *shmNumGxacts;
int    *shmLatestLogFileNo;
int    *shmEarliestLogFileNo;

static int          ControlLockCount=0;
static StringInfoData   errMessage;

char dtm_dir[MAXPGPATH];

/*global transaction array */
static TMGXACT      **shmGxactArray, *currentGxact;

static bool duringRecovery = false;
static int max_tm_gxacts = 100;
static int checkpoint_interval = 5000;

/* for semantic checking - debugging purpose */
static int lastGxactState;
static bool isDirty = false;

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
/*
//#define GET_SEQ_NO(shmGIDSeq)                    (shmGIDSeq & 0x00FFFFFF)
//#define GET_SEQ_NODE_NO(shmGIDSeq)               ((segno & 0xFF000000) >> 24)
*/


int execCommand(void);
void initGxact( TMGXACT *gxact );
TMGXACT *allocGxact(void);
void restoreGxact(TMGXACT *gxact);
void releaseGxact(void);
void updateDtxState(DtxState state);
void generateGID(char *gid);
void shutdownTM(void);
void recoverTM(void);
bool phase2Recovery(void);
void cleanRMResultSets( PGresult **pgresultSets );
bool dumpRMxacts( PGresult **pgresultSets );
bool checkRM( PGresult **pgresultSets );
bool recoverRMs(PGresult ***pgresultSets);
void recoverCommittedDtx(void);
void recoverRollbackedDtx(void);
void getTmLock(void);
void releaseTmLock(void);


/*
 *  starting a global transaction, called by user begin
 *  or by CdbDispatchCommand
 */
int
startDtxTransaction(void)
{
    int ret=DTX_OK;

    if (Mpp_role != MPP_ROLE_DISPATCH)
        return DTX_OK;
    if ( currentGxact == NULL )
    {
        ret = condStartDtxTransaction();
    }
    Assert(currentGxact->infomask == 0);
    return ret;
}

/*
 * committing a global transaction, called by user commit
 * or by CommitTransaction
 */
int
commitDtxTransaction(void)
{
    if (Mpp_role != MPP_ROLE_DISPATCH || currentGxact == NULL)
        return DTX_OK;

	/* temporary set all transaction dirty */
    isDirty = true;

    return condCommitDtxTransaction(!isDirty);
}

/*
 * rollback a global transaction, called by user rollback
 * or by AbortTransaction during Postgres automatic rollback
 */
int
rollbackDtxTransaction(void)
{
    if (Mpp_role != MPP_ROLE_DISPATCH || currentGxact == NULL)
        return DTX_OK;

    /* temporary ignore this call if it is in phase 2 commit, which can not
     * be rolled back. We may need to handle this case for FTS
     */
    if (currentGxact->state == DTX_STATE_2P_COMMIT)
        return DTX_OK;

    return condRollbackDtxTransaction();
}

/*
 * starting a global transaction, call by internal command
 * if transaction is started, this method is a no-op
 */
int
condStartDtxTransaction(void)
{
    if ( currentGxact == NULL )
    {
        currentGxact = allocGxact();
    }
    else
    {
        Assert( currentGxact->state == DTX_STATE_IN_PROGRESS );
        return DTX_OK;
    }

    ereport(DEBUG1,
            (errmsg("condStartDtxTransaction:  %s", currentGxact->gid)));
    /* send additional begin transaction */
    if (execCommand() != DTX_OK)
    {
        errMessage.len = 0;
        ereport(ERROR,
            (errmsg("Global transaction failed on command \"BEGIN %s\" due to error: %s",
                    currentGxact->gid, errMessage.data)));
    }

    return DTX_OK;
}

/*
 * committing a global transaction, called by internal
 * this method does a two-phase commit.
 */
int
condCommitDtxTransaction(bool onephase)
{
    if (currentGxact == NULL)
    {
        return DTX_ERR_PROTO;
    }
    ereport(DEBUG1,
        (errmsg("condCommitDtxTransaction:  %s", currentGxact->gid)));
    if (!onephase)
    {
        /* update the tm state first, so we know the latest state of the txn */
        updateDtxState(DTX_STATE_PREPARED);

        /* send prepare to all RM */
        if (execCommand() != DTX_OK)
        {
            errMessage.len = 0;
            ereport(ERROR,
                (errmsg("Global transaction failed on command \"PREPARE %s\" due to error: %s",
                        currentGxact->gid, errMessage.data)));
        }

        /* set the transaction to be prepared, so it will not be removed before
         * all qe finish.
         */
        currentGxact->infomask |= GXACT_STATUS_PERSISTENT;

        /* send phase 2 commit to all RM */
        updateDtxState(DTX_STATE_2P_COMMIT);
        if (execCommand() != DTX_OK)
        {
            errMessage.len = 0;
            ereport(ERROR,
                (errmsg("Global transaction failed on command \"COMMIT PREPARED %s\" due to error: %s",
                        currentGxact->gid, errMessage.data)));
        }
    }
    else
    {
        /* one phase commit case */
        updateDtxState(DTX_STATE_1P_COMMIT);

        if (execCommand() != DTX_OK)
        {
            errMessage.len = 0;
            ereport(ERROR,
                (errmsg("Global transaction failed on command \"COMMIT %s\" due to error: %s",
                        currentGxact->gid, errMessage.data)));
        }
    }

    /* set txn tm state to be done, so it will be removed from cache */
    updateDtxState(DTX_STATE_COMMIT_DONE);
    return DTX_OK;
}

/*
 * rolling back a global transaction, called by internal
 */
int
condRollbackDtxTransaction(void)
{
    int ret;
    bool ignoreError;
    int previous_state;

    if (currentGxact == NULL)
    {
        return DTX_ERR_PROTO;
    }
    ereport(DEBUG1,
        (errmsg("condRollbackDtxTransaction:  %s", currentGxact->gid)));

    /* keep previous state, the transaction has been aborted or
     * rollback has been called.
     */
    ignoreError = (currentGxact->state == DTX_STATE_ABORT ||
                   currentGxact->state == DTX_STATE_ROLLBACK);
    previous_state = currentGxact->state;

    /* update tm state first */
    updateDtxState(DTX_STATE_ROLLBACK);

    /* send rollback to all RM */
    if (previous_state == DTX_STATE_ROLLBACK ||
        (ret=execCommand()) != DTX_OK)
    {
        char *gid = currentGxact->gid;
        /* clean up gxact even if rollback failed, txn on QE will be
         * automatocally rollback, so we need to clean txn in DTM
         */
        if ( (currentGxact->infomask & GXACT_STATUS_PERSISTENT) == 0 )
            updateDtxState(DTX_STATE_ROLLBACK_DONE);
        else
            /* leave it in the list for recovery */
            currentGxact = NULL;

        /* destroy all gangs to clean up connections */
        disconnectAndDestroyAllGangs();

        /* if the first time failure, throw exception, otherwise keep silent */
        errMessage.len = 0;
        if (!ignoreError)
            ereport(ERROR,
                (errmsg("Global transaction failed on command \"ROLLBACK %s\"", gid),
                 errdetail("%s", errMessage.data)));
    }
    else
        /* remove gxact either rollback is successful or xact is not prepared. */
        updateDtxState(DTX_STATE_ROLLBACK_DONE);

    return ret;
}

/*
 * execute a transaction command on all QE
 */
int
execCommand(void)
{
    char cmdbuf[60];
    struct Gang                *primaryGang = NULL;
    struct Gang                *mirrorGang = NULL;
    SegmentDatabaseDescriptor **failedSegDB=NULL;
    int numOfFailed=0;
    int ret = DTX_OK;

    switch (currentGxact->state)
    {
        case DTX_STATE_IN_PROGRESS:

            if ( XactIsoLevel != DefaultXactIsoLevel )
            {
                char *isoLevel = NULL;
                switch( XactIsoLevel)
                {
                    case XACT_READ_UNCOMMITTED:
                        isoLevel = "ISOLATION LEVEL READ UNCOMMITTED";
                        break;
                    case XACT_READ_COMMITTED:
                        isoLevel = "ISOLATION LEVEL READ COMMITTED";
                        break;
                    case XACT_REPEATABLE_READ:
                        isoLevel = "ISOLATION LEVEL REPEATABLE READ";
                        break;
                    case XACT_SERIALIZABLE:
                        isoLevel = "ISOLATION LEVEL SERIALIZABLE";
                        break;
                }
                sprintf(cmdbuf, "%s'%s' %s", "BEGIN ", currentGxact->gid, isoLevel);
            }
            else
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
            /* error */
            Assert( currentGxact->state == 0 );
            break;
    }

    /* dispatch command */
    cdbdisp_dispatchCommand(cmdbuf, false, false, NULL, &primaryGang, &mirrorGang);

    /* wait for completion; if unsuccessful, handle error.  Don't cancel. */
    if ( !CdbCheckDispatchResultInt(&failedSegDB,&numOfFailed, false) )
    {
        initStringInfo(&errMessage);
        cdbdisp_dumpGangResults(primaryGang, &errMessage, false);
        cdbdisp_dumpGangResults(mirrorGang, &errMessage, false);

        switch (currentGxact->state)
        {
#if 0       /* query plan contains gang info, so no way to retry now. */
            case DTX_STATE_IN_PROGRESS:
                for (i=0;i<numOfFailed;i++)
                {
                    SegmentDatabaseDescriptor *segDB=failedSegDB[i];
                    if (PQstatus(segDB->conn) != CONNECTION_OK)
                    {
                        retry = false;
                        break;
                    }
                }

                if ( retry )
                {
                    / * dispatch command * /
                    CdbDispatchCommandIntEnumeration(cmdbuf, failedSegDB, numOfFailed, false, NULL);
                    results = CdbCheckDispatchResult();
                }
                if ( !results->success )
                {
                    updateDtxState( DTX_STATE_ABORT );
                    ret = DTX_ERR_COMM;
                }
                break;
#endif
            case DTX_STATE_IN_PROGRESS:
                updateDtxState( DTX_STATE_ABORT );
                ret = DTX_ERR_COMM;
                break;
            case DTX_STATE_PREPARED:
            case DTX_STATE_ROLLBACK:
            case DTX_STATE_1P_COMMIT:
                updateDtxState( DTX_STATE_ABORT );
                ret = DTX_ERR_COMM;
                break;
            case DTX_STATE_2P_COMMIT:
                /* keep this transaction for recovery */
                ret = DTX_ERR_COMM;
                break;
            default:
                /* error */
                Assert( currentGxact->state == 0 );
                ret = DTX_ERR_COMM;
                break;
        }
    }

    /* Free the broken connection list. */
    if (failedSegDB)
        pfree(failedSegDB);

    return ret;
}

/*
 * start scan of current active global transactions.
 */
int
startTMXactScan(void)
{
    getTmLock();
    return *shmNumGxacts;
}

/*
 * scaning current active global transaction.
 */
TMGXACT *
getNextTMXact(int index)
{
    return shmGxactArray[index];
}

/*
 * end scaning current active global transaction.
 */
void
endTMXactScan(void)
{
    releaseTmLock();
}

/*
 * initialize TM, called by CdbInit for each process.
 * first call to this method will trigger tm recovery.
 */
void
initTM(void)
{
    /* do not init errMessage here, this is in a different context.
    initStringInfo(&errMessage);
    */
    if (!*shmTmRecoverred)
    {
        getTmLock();
        if (!*shmTmRecoverred)
        {
            recoverTM();
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

/* get tm share memory size */
int
tmShmemSize(void)
{
    if (Mpp_role != MPP_ROLE_DISPATCH)
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
    bool found;
    TmControlBlock *shared;

    if (Mpp_role != MPP_ROLE_DISPATCH)
        return;

    shared = (TmControlBlock *)ShmemInitStruct("Transaction manager", tmShmemSize(), &found);
    if(!shared)
        elog(FATAL, "could not initialize transaction manager share memory");

    if (!IsUnderPostmaster)
            /* Initialize locks and shared memory area */
    {
        int i=0;
        TMGXACT *gxact = NULL;

        shared->ControlLock = LWLockAssign();
        shmControlLock = shared->ControlLock;
        shmTmRecoverred  = &shared->recoverred;
        shmGIDSeq = &shared->seqno;
        shmLatestLogFileNo = &shared->latestLogFileNo;
        shmEarliestLogFileNo = &shared->earliestLogFileNo;
        shmNumGxacts  = &shared->num_active_xacts;
        shmGxactArray = shared->gxact_array;

        // initialize gxact array
        gxact = (TMGXACT *)(shmGxactArray + max_tm_gxacts);
        for ( i=0;i<max_tm_gxacts;i++ )
            shmGxactArray[i] = gxact++;
        *shmEarliestLogFileNo = *shmLatestLogFileNo = -1;
    }

    /* Init directory path */
    snprintf(dtm_dir, MAXPGPATH, "%s/pg_dtm", DataDir);
}

/*
 * is global transaction started
 */
bool
isInGlobalTransaction(void)
{
    if ( Mpp_role != MPP_ROLE_DISPATCH )
        return false;

    /* check fts sync count */
    if ( !verifyFtsSyncCount() )
    {
        /* recofig mpp connections */
        if ( !FtsReConfigureMPP() )
            elog(PANIC, "FTS: reconfigMPP failed");
    }
    return (currentGxact!=NULL);
}

/* initialize a global transaction context */
void
initGxact( TMGXACT *gxact )
{
    generateGID(gxact->gid);
    gxact->state = DTX_STATE_IN_PROGRESS;
    gxact->infomask = 0;
    gxact->fileno = 0;
    gxact->offset = 0;
}

/*
 * allocate a global transaction context from share memory.
 */
TMGXACT *
allocGxact(void)
{
	TMGXACT *gxact;
    getTmLock();

    if ( *shmNumGxacts >= max_tm_gxacts )
    {
        releaseTmLock();
        ereport(ERROR,
                    (errmsg("can not allocate global transaction")));
    }

    gxact = shmGxactArray[(*shmNumGxacts)++];
    initGxact( gxact );
    releaseTmLock();

    return gxact;
}

/*
 * restore global transaction during tm log recovery
 */
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

/*
 * redo transaction log
 */
void
redoTmLog(TmLogRecord *glog)
{
    int i;

    for (i=0;i<*shmNumGxacts;i++)
    {
        if (strcmp(glog->header.gid, shmGxactArray[i]->gid) == 0)
        {
            /* found an active global transaction */
            currentGxact = shmGxactArray[i];
            break;
        }
    }
    if ( i==*shmNumGxacts )
    {
        /* transaction not found, this is the first log of this transaction,
         * amd it should be a prepare log, rollback log or 1p commit log.
         */
        Assert(glog->header.state == DTX_STATE_PREPARED ||
               glog->header.state == DTX_STATE_1P_COMMIT ||
               glog->header.state == DTX_STATE_ROLLBACK);
        restoreGxact( NULL );
        memcpy(currentGxact->gid, glog->header.gid, TMGIDSIZE);
    }

    /* use set state instead of update state, so log will not written again */
    SET_DTX_STATE(currentGxact, glog->header.state);
    currentGxact->fileno = glog->header.thisFileNo;
    currentGxact->offset = glog->header.thisOffset;

    /* release it if it's done */
    if (currentGxact->state == DTX_STATE_ROLLBACK_DONE ||
        currentGxact->state == DTX_STATE_COMMIT_DONE)
        releaseGxact();
}

/*
 * release global transaction to share memory
 */
void
releaseGxact(void)
{
    int i;

    getTmLock();
    /* find slot of current transaction */

    for(i=0; i < *shmNumGxacts; i++)
        if(shmGxactArray[i] == currentGxact)
            break;

    /* move this to the next available slot */
    (*shmNumGxacts)--;
    if (i!=*shmNumGxacts)
    {
        shmGxactArray[i] = shmGxactArray[*shmNumGxacts];
        shmGxactArray[*shmNumGxacts] = currentGxact;
    }
    currentGxact = NULL;
    releaseTmLock();
    isDirty = false;
}


/*
 * update transaction state, this method writes transaction and checkpoint log
 */
void
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
                /* internal error, shouldn't happen */
                ereport(ERROR, (errmsg("updateDtxState: error state not compatiable, CST: %d, NST: %d",
                                gxact->state, state)));
            }
            break;
        case    DTX_STATE_ABORT:
        case    DTX_STATE_COMMIT_DONE:
        case    DTX_STATE_ROLLBACK_DONE:
        default:
            break;
    }

    /* write tm log */
    if ( state != DTX_STATE_ABORT )
    {
        TmLogRecord record={{0,0,"",TM_LOG_XACT,0,0}};
        strcpy(record.header.gid, gxact->gid);
        record.header.state = state;

        /* synchronizing log file write */
        getTmLock();
        writeTmLogRecord(&record);
        currentGxact->fileno = record.header.thisFileNo;
        currentGxact->offset = record.header.thisOffset;
        releaseTmLock();
    }

    /* set memory state */
    SET_DTX_STATE(gxact, state);

    getTmLock();
    if (state == DTX_STATE_COMMIT_DONE || state == DTX_STATE_ROLLBACK_DONE)
        releaseGxact();

    /* checkpoint */
    if ( !duringRecovery && ((*shmGIDSeq) % checkpoint_interval) == 0 )
    {
        checkPointTm();
        (*shmGIDSeq)++;
    }
    else
        flushTmLogFile();
    releaseTmLock();

    lastGxactState = state;
}

/* generate global transaction id */
void
generateGID(char *gid)
{
    time_t t = time(NULL);
    if (t == (time_t)-1)
    {
        ereport(LOG,
            (errmsg("can not generate global id properly")));
    }

    /* tm lock acquired by caller */
    sprintf( gid, "%ld-%.10d", t, (*shmGIDSeq)++);
}

/* shutdown tm, called when process exits */
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

/*
 * perform TM recovery, this connects to all QE and resolve all in-doubt txn.
 */
void
recoverTM(void)
{
    PGresult **pgresultSets;

    duringRecovery = true;

    /* intialize fts sync count */
    verifyFtsSyncCount();

    /* phase 1 recovery
     * read and roll forward tm log
     */
    recoverTmLog();

    /* phase 2 recovery
     * go through all in-double transactions and resolve them
     */
    if ( !phase2Recovery() )
        return;

    /* check any un-resolved transactions in TM */
    if ( *shmNumGxacts != 0 )
    {
        /* still have in-doubt transactions, save it to log */
        int i=0;
        for ( i=0;i<*shmNumGxacts;i++ )
            ereport(LOG,
                    (errmsg("global xact %s pending on TM", shmGxactArray[i]->gid)));
        ereport(FATAL,
                (errmsg("global xact is not clean in TM")));
    }

    /* check again to see any un-resolved transactions in QE */
    if ( recoverRMs( &pgresultSets ) )
    {
        /* still have in-doubt transactions in QE, save it to log */
        dumpRMxacts( pgresultSets );
        ereport(FATAL,
            (errmsg("TM recovery failure, in-doubt transaction left on QE")));
    }

    cleanRMResultSets( pgresultSets );

    /* finish recovery, do checkpoint */
    checkPointTm();
    duringRecovery = false;
}

/*
 * phase two recovery
 */
bool
phase2Recovery(void)
{
    /* get all in-doubt transactions from QEs */
    PGresult **pgresultSets;
    bool needRecovery = recoverRMs( &pgresultSets );
    int i=0;

    /* check to see if any in-doubt transaction exists */
    if ( *shmNumGxacts == 0 && !needRecovery )
    {
        checkPointTm();
        duringRecovery = false;
        cleanRMResultSets( pgresultSets );
        currentGxact = NULL;
        return false;
    }

    /* recover in-doubt transaction in TM */
    for ( i=0;i<*shmNumGxacts;i++ )
    {
        currentGxact = shmGxactArray[i];

        switch (currentGxact->state)
        {
            case DTX_STATE_IN_PROGRESS:
            case DTX_STATE_PREPARED:
            case DTX_STATE_1P_COMMIT:
            case DTX_STATE_ROLLBACK:
                /* reset persistent flag, so currentGxact will be cleaned */
                currentGxact->infomask = 0;
                if ( checkRM( pgresultSets ) )
                    recoverRollbackedDtx();
                else
                    updateDtxState(DTX_STATE_ROLLBACK_DONE);
                break;

            case DTX_STATE_2P_COMMIT:
                if ( checkRM( pgresultSets ) )
                    recoverCommittedDtx();
                else
                    updateDtxState(DTX_STATE_COMMIT_DONE);
                break;

            case DTX_STATE_ABORT:
            case DTX_STATE_COMMIT_DONE:
            case DTX_STATE_ROLLBACK_DONE:
            default:
                ereport(LOG,
                    (errmsg("invalid state during recovery gid: %s, state: %d", currentGxact->gid, currentGxact->state)));
                releaseGxact();
                break;
        }

        i--;
    }

    cleanRMResultSets( pgresultSets );
    currentGxact = NULL;

    return true;
}

/* free pgresultset */
void
cleanRMResultSets( PGresult **pgresultSets )
{
    /* TODO: need to handle multiple dbs in a segment case */
    /*
     int tolaldbs = CdbClusterDefinition->segment_count * CdbClusterDefinition->db_count;
     */
    int i=0;
    PGresult *rs = pgresultSets[0];
    do
    {
        PQclear(rs);
    } while ((rs = pgresultSets[++i]) != NULL);
    free(pgresultSets);
}

/* dump all in-doubt transaction in RM */
bool
dumpRMxacts( PGresult **pgresultSets )
{
    int i=0,j;

    PGresult *rs = pgresultSets[0];
    do
    {
        int rows = PQntuples(rs);
        {
            for (j=0;j<rows;j++)
            {
                ereport(ERROR,
                    (errmsg("global xact %s pending on QE%d and not found in TM",
                        PQgetvalue(rs,j,0),i)));
            }
        }
    } while ((rs = pgresultSets[++i]) != NULL);

    return false;
}

/*
 * checkRM - check recovery is required on RM
 */
bool
checkRM( PGresult **pgresultSets )
{
    int i=0,j;

    PGresult *rs = pgresultSets[0];
    do
    {
        int rows = PQntuples(rs);
        {
            for (j=0;j<rows;j++)
            {
                if ( strcmp(currentGxact->gid, PQgetvalue(rs,j,0)) == 0 )
                    return true;
            }
        }
    } while ((rs = pgresultSets[++i]) != NULL);

    return false;
}

/*
 * recoverRMs - collect all prepared transactions from RM
 */
bool
recoverRMs(PGresult ***pgresultSets)
{
    char cmdbuf[] = "select gid from pg_prepared_xacts";
    int i;
    PGresult *rs;

    /* call to all QE to get in-doubt transactions */
    PG_TRY();
    {
        CdbDoCommandInt(cmdbuf, false, pgresultSets);
    }
    PG_CATCH();
    {
        /* need recovery */
        return true;
    }
    PG_END_TRY();

    i=0;
    rs = (*pgresultSets)[0];
    do
    {
        if (PQntuples(rs) != 0)
            /* need recovery */
            return true;

    } while ((rs = (*pgresultSets)[++i]) != NULL);

    /* do not need recovery */
    return false;
}

/* rollback in-doubt transaction */
void
recoverRollbackedDtx(void)
{
    /* send second phase commit to QE */
    ereport(DEBUG1,
        (errmsg("recovery: rollback DtxTransaction:  %s", currentGxact->gid)));
    updateDtxState(DTX_STATE_ROLLBACK);
    /* ignore the error during recovery */
    execCommand();
    updateDtxState(DTX_STATE_ROLLBACK_DONE);
}

/* commit in-doubt transaction */
void
recoverCommittedDtx(void)
{
    /* send second phase commit to QE */
    ereport(DEBUG1,
        (errmsg("recovery: commit phase 2 DtxTransaction:  %s", currentGxact->gid)));
    /* ignore the error during recovery */
    execCommand();
    updateDtxState(DTX_STATE_COMMIT_DONE);
}

/* acquire tm lw lock */
void
getTmLock(void)
{
    if ( ControlLockCount++ == 0 )
        LWLockAcquire(shmControlLock, LW_EXCLUSIVE);
}

/* release tm lw lock */
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

/*
 * discardDtxTransaction: called by fts to discard it
 */
void
discardDtxTransaction(void)
{
    if (currentGxact != NULL)
    {
        if (currentGxact->state == DTX_STATE_2P_COMMIT)
        {
            // has to commit the transaction
            recoverCommittedDtx();
        }
        else if (currentGxact->state == DTX_STATE_PREPARED)
        {
            /* update tm state first and send rollback to all qe */
            updateDtxState(DTX_STATE_ABORT);
            condRollbackDtxTransaction();
        }
        else
        {
            /* discard the transaction, update tm state only.
             * in this case, all connection should be restarted and
             * this global transaction should be automatically rollback
             */
            updateDtxState(DTX_STATE_ROLLBACK);
            updateDtxState(DTX_STATE_ROLLBACK_DONE);
        }
    }
}

bool
isTMInRecovery(void)
{
    return duringRecovery;
}

void
setDtxDirty(void)
{
    isDirty = true;
}
