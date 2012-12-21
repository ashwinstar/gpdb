/*
 * Auto-generated Mocking Source
 * 
 * Source: cdb/cdbpersistentrecovery.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "postgres.h"
#include "cdb/cdbdoublylinked.h"
#include "access/persistentfilesysobjname.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbpersistentstore.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "cdb/cdbpersistentrelation.h"
#include "cdb/cdbpersistentrecovery.h"
#include "access/heapam.h"
#include "catalog/pg_tablespace.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "cdb/cdbdoublylinked.h"
#include "utils/hsearch.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "cdb/cdbdirectopen.h"
#include "utils/guc.h"
#include "storage/smgr.h"
#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbresynchronizechangetracking.h"
#include "access/twophase.h"

Pass2RecoveryHashShmem_s *pass2RecoveryHashShmem;
void 
Pass2Recovery_ShmemInit(void)
{



    mock();

}

Size 
Pass2Recovery_ShmemSize(void)
{



    return (Size) mock();

}

bool 
PersistentRecovery_ShouldHandlePass1XLogRec(XLogRecPtr * beginLoc, XLogRecPtr * lsn, XLogRecord * record)
{

    check_expected(beginLoc);
    check_expected(lsn);
    check_expected(record);
    optional_assignment(beginLoc);
    optional_assignment(lsn);
    optional_assignment(record);
    return (bool) mock();

}

void 
PersistentRecovery_HandlePass2XLogRec(XLogRecPtr * beginLoc, XLogRecPtr * lsn, XLogRecord * record)
{

    check_expected(beginLoc);
    check_expected(lsn);
    check_expected(record);
    optional_assignment(beginLoc);
    optional_assignment(lsn);
    optional_assignment(record);
    mock();

}

bool 
PersistentRecovery_ShouldHandlePass3XLogRec(XLogRecPtr * beginLoc, XLogRecPtr * lsn, XLogRecord * record)
{

    check_expected(beginLoc);
    check_expected(lsn);
    check_expected(record);
    optional_assignment(beginLoc);
    optional_assignment(lsn);
    optional_assignment(record);
    return (bool) mock();

}

void 
PersistentRecovery_CompletePass(void)
{



    mock();

}

void 
PersistentRecovery_Scan(void)
{



    mock();

}

void 
PersistentRecovery_CrashAbort(void)
{



    mock();

}

void 
PersistentRecovery_Update(void)
{



    mock();

}

void 
PersistentRecovery_Drop(void)
{



    mock();

}

void 
PersistentRecovery_UpdateAppendOnlyMirrorResyncEofs(void)
{



    mock();

}

void 
PersistentRecovery_SerializeRedoRelationFile(int redoRelationFile)
{

    check_expected(redoRelationFile);

    mock();

}

void 
PersistentRecovery_DeserializeRedoRelationFile(int redoRelationFile)
{

    check_expected(redoRelationFile);

    mock();

}
