/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/bufmgr.h, storage/buf_internals.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <sys/file.h>
#include <unistd.h>
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "storage/buf_internals.h"
#include "storage/bufpage.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/resowner.h"
#include "sys/mman.h"
#include "access/heapam.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbfilerepprimary.h"
#include "access/aosegfiles.h"
#include "access/aocssegfiles.h"
#include "cdb/cdbappendonlyam.h"

volatile BufferDesc *BufferDescriptors;
BufferDesc     *LocalBufferDescriptors;
bool            strategy_hint_vacuum;
long int        ReadBufferCount;
long int        ReadLocalBufferCount;
long int        BufferHitCount;
long int        LocalBufferHitCount;
long int        BufferFlushCount;
long int        LocalBufferFlushCount;
volatile BufferDesc *
StrategyGetBuffer(void)
{



    return (BufferDesc *) mock();

}

void 
StrategyFreeBuffer(volatile BufferDesc * buf, bool at_head)
{

    check_expected(buf);
    check_expected(at_head);
    optional_assignment(buf);
    mock();

}

int 
StrategySyncStart(void)
{



    return (int) mock();

}

Size 
StrategyShmemSize(void)
{



    return (Size) mock();

}

void 
StrategyInitialize(bool init)
{

    check_expected(init);

    mock();

}

Size 
BufTableShmemSize(int size)
{

    check_expected(size);

    return (Size) mock();

}

void 
InitBufTable(int size)
{

    check_expected(size);

    mock();

}

uint32 
BufTableHashCode(BufferTag * tagPtr)
{

    check_expected(tagPtr);
    optional_assignment(tagPtr);
    return (uint32) mock();

}

int 
BufTableLookup(BufferTag * tagPtr, uint32 hashcode)
{

    check_expected(tagPtr);
    check_expected(hashcode);
    optional_assignment(tagPtr);
    return (int) mock();

}

int 
BufTableInsert(BufferTag * tagPtr, uint32 hashcode, int buf_id)
{

    check_expected(tagPtr);
    check_expected(hashcode);
    check_expected(buf_id);
    optional_assignment(tagPtr);
    return (int) mock();

}

void 
BufTableDelete(BufferTag * tagPtr, uint32 hashcode)
{

    check_expected(tagPtr);
    check_expected(hashcode);
    optional_assignment(tagPtr);
    mock();

}

BufferDesc     *
LocalBufferAlloc_SMgr(SMgrRelation reln, BlockNumber blockNum, bool * foundPtr)
{

    check_expected(reln);
    check_expected(blockNum);
    check_expected(foundPtr);
    optional_assignment(foundPtr);
    return (BufferDesc *) mock();

}

void 
MarkLocalBufferDirty(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

void 
DropRelFileNodeLocalBuffers(RelFileNode rnode, BlockNumber firstDelBlock)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);
    check_expected(firstDelBlock);

    mock();

}

void 
AtEOXact_LocalBuffers(bool isCommit)
{

    check_expected(isCommit);

    mock();

}

int             NBuffers;
bool            memory_protect_buffer_pool;
bool            flush_buffer_pages_when_evicted;
bool            zero_damaged_pages;
double          bgwriter_lru_percent;
double          bgwriter_all_percent;
int             bgwriter_lru_maxpages;
int             bgwriter_all_maxpages;
bool            IsUnderPostmaster;
char           *BufferBlocks;
int32          *PrivateRefCount;
int             NLocBuffer;
Block          *LocalBufferBlockPointers;
int32          *LocalRefCount;
Buffer 
ReadBuffer_ResyncEx(SMgrRelation reln, BlockNumber blockNum)
{

    check_expected(reln);
    check_expected(blockNum);

    return (Buffer) mock();

}

Buffer 
ReadBuffer(Relation reln, BlockNumber blockNum)
{

    check_expected(reln);
    check_expected(blockNum);

    return (Buffer) mock();

}

Buffer 
ReadBuffer_Ex_SMgr(SMgrRelation smgr, BlockNumber blockNum, volatile BufferDesc * availBufHdr, bool isLocalBuf, bool isTemp)
{

    check_expected(smgr);
    check_expected(blockNum);
    check_expected(availBufHdr);
    check_expected(isLocalBuf);
    check_expected(isTemp);
    optional_assignment(availBufHdr);
    return (Buffer) mock();

}

void 
ReleaseBuffer(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

void 
ReleaseBuffer_Ex(Buffer buffer, bool flushIfDirty)
{

    check_expected(buffer);
    check_expected(flushIfDirty);

    mock();

}

void 
UnlockReleaseBuffer(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

void 
MarkBufferDirty(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

void 
IncrBufferRefCount(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

Buffer 
ReleaseAndReadBuffer(Buffer buffer, Relation relation, BlockNumber blockNum)
{

    check_expected(buffer);
    check_expected(relation);
    check_expected(blockNum);

    return (Buffer) mock();

}

Buffer 
KillAndReadBuffer(Buffer buffer, Relation relation, BlockNumber blockNum)
{

    check_expected(buffer);
    check_expected(relation);
    check_expected(blockNum);

    return (Buffer) mock();

}

void 
InitBufferPool(void)
{



    mock();

}

void 
InitBufferPoolAccess(void)
{



    mock();

}

void 
InitBufferPoolBackend(void)
{



    mock();

}

char           *
ShowBufferUsage(void)
{



    return (char *) mock();

}

void 
ResetBufferUsage(void)
{



    mock();

}

void 
AtEOXact_Buffers(bool isCommit)
{

    check_expected(isCommit);

    mock();

}

void 
PrintBufferLeakWarning(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

void 
FlushBufferPool(void)
{



    mock();

}

BlockNumber 
BufferGetBlockNumber(Buffer buffer)
{

    check_expected(buffer);

    return (BlockNumber) mock();

}

BlockNumber 
RelationGetNumberOfBlocks(Relation relation)
{

    check_expected(relation);

    return (BlockNumber) mock();

}

void 
RelationTruncate(Relation rel, BlockNumber nblocks, bool markPersistentAsPhysicallyTruncated)
{

    check_expected(rel);
    check_expected(nblocks);
    check_expected(markPersistentAsPhysicallyTruncated);

    mock();

}

void 
FlushRelationBuffers(Relation rel)
{

    check_expected(rel);

    mock();

}

void 
DropRelFileNodeBuffers(RelFileNode rnode, bool istemp, BlockNumber firstDelBlock)
{

    check_expected(rnode.spcNode);
    check_expected(rnode.dbNode);
    check_expected(rnode.relNode);
    check_expected(istemp);
    check_expected(firstDelBlock);

    mock();

}

void 
DropDatabaseBuffers(Oid tbpoid, Oid dbid)
{

    check_expected(tbpoid);
    check_expected(dbid);

    mock();

}

Size 
BufferShmemSize(void)
{



    return (Size) mock();

}

RelFileNode 
BufferGetFileNode(Buffer buffer)
{

    check_expected(buffer);

    mock();
    RelFileNode     mock_tmp;
    memset(&mock_tmp, 0, sizeof(mock_tmp));
    return mock_tmp;


}

void 
SetBufferCommitInfoNeedsSave(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

void 
UnlockBuffers(void)
{



    mock();

}

void 
LockBuffer(Buffer buffer, int mode)
{

    check_expected(buffer);
    check_expected(mode);

    mock();

}

bool 
ConditionalLockBuffer(Buffer buffer)
{

    check_expected(buffer);

    return (bool) mock();

}

void 
LockBufferForCleanup(Buffer buffer)
{

    check_expected(buffer);

    mock();

}

void 
AbortBufferIO(void)
{



    mock();

}

void 
BufmgrCommit(void)
{



    mock();

}

void 
BufferSync(void)
{



    mock();

}

void 
BgBufferSync(void)
{



    mock();

}

void 
AtProcExit_LocalBuffers(void)
{



    mock();

}

void 
StrategyHintVacuum(bool vacuum_active)
{

    check_expected(vacuum_active);

    mock();

}
