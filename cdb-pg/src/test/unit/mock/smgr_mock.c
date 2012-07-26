/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/smgr.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "access/persistentfilesysobjname.h"
#include "access/xact.h"
#include "access/xlogmm.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_resqueue.h"
#include "catalog/pg_tablespace.h"
#include "cdb/cdbfilerepprimary.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentrecovery.h"
#include "cdb/cdbmirroredappendonly.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "commands/filespace.h"
#include "commands/tablespace.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "cdb/cdbtm.h"
#include "access/twophase.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <glob.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

char           *
StorageManagerMirrorMode_Name(StorageManagerMirrorMode mirrorMode)
{

    check_expected(mirrorMode);

    return (char *) (unsigned) mock();

}

void 
smgrinit(void)
{



    mock();

}

SMgrRelation 
smgropen(RelFileNode rnode)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);

    return (SMgrRelation) mock();

}

void 
smgrsetowner(SMgrRelation * owner, SMgrRelation reln)
{

    check_expected(owner);
    check_expected(reln);
    optional_assignment(owner);
    mock();

}

void 
smgrclose(SMgrRelation reln)
{

    check_expected(reln);

    mock();

}

void 
smgrcloseall(void)
{



    mock();

}

void 
smgrclosenode(RelFileNode rnode)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);

    mock();

}

void 
smgrcreatefilespacedirpending(Oid filespaceOid, int16 primaryDbId, char *primaryFilespaceLocation, int16 mirrorDbId, char *mirrorFilespaceLocation, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(filespaceOid);
    check_expected(primaryDbId);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorDbId);
    check_expected(mirrorFilespaceLocation);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
smgrcreatefilespacedir(Oid filespaceOid, char *primaryFilespaceLocation, char *mirrorFilespaceLocation, StorageManagerMirrorMode mirrorMode, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    check_expected(mirrorMode);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrcreatetablespacedirpending(TablespaceDirNode * tablespaceDirNode, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(tablespaceDirNode);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(tablespaceDirNode);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
smgrcreatetablespacedir(Oid tablespaceOid, StorageManagerMirrorMode mirrorMode, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(tablespaceOid);
    check_expected(mirrorMode);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrcreatedbdirjustintime(DbDirNode * justInTimeDbDirNode, MirroredObjectExistenceState mirrorExistenceState, StorageManagerMirrorMode mirrorMode, ItemPointer persistentTid, int64 * persistentSerialNum, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(justInTimeDbDirNode);
    check_expected(mirrorExistenceState);
    check_expected(mirrorMode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(justInTimeDbDirNode);
    optional_assignment(persistentSerialNum);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrcreatedbdirpending(DbDirNode * dbDirNode, MirroredObjectExistenceState mirrorExistenceState, ItemPointer persistentTid, int64 * persistentSerialNum, bool flushToXLog)
{

    check_expected(dbDirNode);
    check_expected(mirrorExistenceState);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(flushToXLog);
    optional_assignment(dbDirNode);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
smgrcreatedbdir(DbDirNode * dbDirNode, StorageManagerMirrorMode mirrorMode, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(dbDirNode);
    check_expected(mirrorMode);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(dbDirNode);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrcreatepending(RelFileNode * relFileNode, int32 segmentFileNum, PersistentFileSysRelStorageMgr relStorageMgr, PersistentFileSysRelBufpoolKind relBufpoolKind, MirroredObjectExistenceState mirrorExistenceState, MirroredRelDataSynchronizationState relDataSynchronizationState, char *relationName, ItemPointer persistentTid, int64 * persistentSerialNum, bool isLocalBuf, bool bufferPoolBulkLoad, bool flushToXLog)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relStorageMgr);
    check_expected(relBufpoolKind);
    check_expected(mirrorExistenceState);
    check_expected(relDataSynchronizationState);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(isLocalBuf);
    check_expected(bufferPoolBulkLoad);
    check_expected(flushToXLog);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(persistentSerialNum);
    mock();

}

void 
smgrcreate(SMgrRelation reln, bool isLocalBuf, char *relationName, MirrorDataLossTrackingState mirrorDataLossTrackingState, int64 mirrorDataLossTrackingSessionNum, bool ignoreAlreadyExists, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(reln);
    check_expected(isLocalBuf);
    check_expected(relationName);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    check_expected(ignoreAlreadyExists);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relationName);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrrecreate(RelFileNode * relFileNode, bool isLocalBuf, char *relationName, MirrorDataLossTrackingState mirrorDataLossTrackingState, int64 mirrorDataLossTrackingSessionNum, int *primaryError, bool * mirrorDataLossOccurred)
{

    check_expected(relFileNode);
    check_expected(isLocalBuf);
    check_expected(relationName);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    check_expected(primaryError);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(primaryError);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrscheduleunlink(RelFileNode * relFileNode, int32 segmentFileNum, PersistentFileSysRelStorageMgr relStorageMgr, bool isLocalBuf, char *relationName, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relStorageMgr);
    check_expected(isLocalBuf);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    mock();

}

void 
smgrdounlink(RelFileNode * relFileNode, bool isLocalBuf, char *relationName, bool primaryOnly, bool isRedo, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(relFileNode);
    check_expected(isLocalBuf);
    check_expected(relationName);
    check_expected(primaryOnly);
    check_expected(isRedo);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrschedulermfilespacedir(Oid filespaceOid, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(filespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);

    mock();

}

void 
smgrschedulermtablespacedir(Oid tablespaceOid, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(tablespaceOid);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);

    mock();

}

void 
smgrschedulermdbdir(DbDirNode * dbDirNode, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(dbDirNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(dbDirNode);
    mock();

}

void 
smgrdormfilespacedir(Oid filespaceOid, char *primaryFilespaceLocation, char *mirrorFilespaceLocation, bool primaryOnly, bool mirrorOnly, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(filespaceOid);
    check_expected(primaryFilespaceLocation);
    check_expected(mirrorFilespaceLocation);
    check_expected(primaryOnly);
    check_expected(mirrorOnly);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(primaryFilespaceLocation);
    optional_assignment(mirrorFilespaceLocation);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrdormtablespacedir(Oid tablespaceOid, bool primaryOnly, bool mirrorOnly, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(tablespaceOid);
    check_expected(primaryOnly);
    check_expected(mirrorOnly);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrdormdbdir(DbDirNode * dropDbDirNode, bool primaryOnly, bool mirrorOnly, bool ignoreNonExistence, bool * mirrorDataLossOccurred)
{

    check_expected(dropDbDirNode);
    check_expected(primaryOnly);
    check_expected(mirrorOnly);
    check_expected(ignoreNonExistence);
    check_expected(mirrorDataLossOccurred);
    optional_assignment(dropDbDirNode);
    optional_assignment(mirrorDataLossOccurred);
    mock();

}

void 
smgrextend(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{

    check_expected(reln);
    check_expected(blocknum);
    check_expected(buffer);
    check_expected(isTemp);
    optional_assignment(buffer);
    mock();

}

void 
smgrread(SMgrRelation reln, BlockNumber blocknum, char *buffer)
{

    check_expected(reln);
    check_expected(blocknum);
    check_expected(buffer);
    optional_assignment(buffer);
    mock();

}

void 
smgrwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{

    check_expected(reln);
    check_expected(blocknum);
    check_expected(buffer);
    check_expected(isTemp);
    optional_assignment(buffer);
    mock();

}

BlockNumber 
smgrnblocks(SMgrRelation reln)
{

    check_expected(reln);

    return (BlockNumber) mock();

}

BlockNumber 
smgrtruncate(SMgrRelation reln, BlockNumber nblocks, bool isTemp, bool isLocalBuf, ItemPointer persistentTid, int64 persistentSerialNum)
{

    check_expected(reln);
    check_expected(nblocks);
    check_expected(isTemp);
    check_expected(isLocalBuf);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);

    return (BlockNumber) mock();

}

bool 
smgrgetpersistentinfo(XLogRecord * record, RelFileNode * relFileNode, ItemPointer persistentTid, int64 * persistentSerialNum)
{

    check_expected(record);
    check_expected(relFileNode);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    optional_assignment(record);
    optional_assignment(relFileNode);
    optional_assignment(persistentSerialNum);
    return (bool) mock();

}

void 
smgrimmedsync(SMgrRelation reln)
{

    check_expected(reln);

    mock();

}

void 
smgrappendonlymirrorresynceofs(RelFileNode * relFileNode, int32 segmentFileNum, char *relationName, ItemPointer persistentTid, int64 persistentSerialNum, bool mirrorCatchupRequired, MirrorDataLossTrackingState mirrorDataLossTrackingState, int64 mirrorDataLossTrackingSessionNum, int64 mirrorNewEof)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relationName);
    check_expected(persistentTid);
    check_expected(persistentSerialNum);
    check_expected(mirrorCatchupRequired);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    check_expected(mirrorNewEof);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    mock();

}

bool 
smgrgetappendonlyinfo(RelFileNode * relFileNode, int32 segmentFileNum, char *relationName, bool * mirrorCatchupRequired, MirrorDataLossTrackingState * mirrorDataLossTrackingState, int64 * mirrorDataLossTrackingSessionNum)
{

    check_expected(relFileNode);
    check_expected(segmentFileNum);
    check_expected(relationName);
    check_expected(mirrorCatchupRequired);
    check_expected(mirrorDataLossTrackingState);
    check_expected(mirrorDataLossTrackingSessionNum);
    optional_assignment(relFileNode);
    optional_assignment(relationName);
    optional_assignment(mirrorCatchupRequired);
    optional_assignment(mirrorDataLossTrackingState);
    optional_assignment(mirrorDataLossTrackingSessionNum);
    return (bool) mock();

}

int 
smgrGetPendingFileSysWork(EndXactRecKind endXactRecKind, PersistentEndXactFileSysActionInfo ** ptr)
{

    check_expected(endXactRecKind);
    check_expected(ptr);
    optional_assignment(ptr);
    return (int) mock();

}

int 
smgrGetAppendOnlyMirrorResyncEofs(EndXactRecKind endXactRecKind, PersistentEndXactAppendOnlyMirrorResyncEofs ** ptr)
{

    check_expected(endXactRecKind);
    check_expected(ptr);
    optional_assignment(ptr);
    return (int) mock();

}

bool 
smgrIsPendingFileSysWork(EndXactRecKind endXactRecKind)
{

    check_expected(endXactRecKind);

    return (bool) mock();

}

bool 
smgrIsAppendOnlyMirrorResyncEofs(EndXactRecKind endXactRecKind)
{

    check_expected(endXactRecKind);

    return (bool) mock();

}

void 
AtSubCommit_smgr(void)
{



    mock();

}

void 
AtSubAbort_smgr(void)
{



    mock();

}

void 
AtEOXact_smgr(bool forCommit)
{

    check_expected(forCommit);

    mock();

}

void 
PostPrepare_smgr(void)
{



    mock();

}

void 
smgrcommit(void)
{



    mock();

}

void 
smgrabort(void)
{



    mock();

}

void 
smgrsync(void)
{



    mock();

}

void 
smgr_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord * record)
{

    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(lsn.xlogid);
    check_expected(lsn.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}

void 
smgr_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord * record)
{

    check_expected(buf);
    check_expected(beginLoc.xlogid);
    check_expected(beginLoc.xrecoff);
    check_expected(record);
    optional_assignment(record);
    mock();

}

Datum 
smgrout(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

Datum 
smgrin(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

Datum 
smgreq(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}

Datum 
smgrne(FunctionCallInfo fcinfo)
{

    check_expected(fcinfo);

    return (Datum) mock();

}
