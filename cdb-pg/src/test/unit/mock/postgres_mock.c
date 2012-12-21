/*
 * Auto-generated Mocking Source
 * 
 * Source: postgres.h, miscadmin.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "gpmon/gpmon.h"
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <getopt.h>
#include "access/distributedlog.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/async.h"
#include "commands/prepare.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"	/* Slice, SliceTable */
#include "nodes/print.h"
#include "optimizer/planner.h"
#include "pgstat.h"
#include "pg_trace.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/flatfiles.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/datum.h"
#include "utils/debugbreak.h"
#include "mb/pg_wchar.h"
#include "cdb/cdbvars.h"
#include "cdb/cdblogsync.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbdtxcontextinfo.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbgang.h"
#include "cdb/ml_ipc.h"
#include "utils/guc.h"
#include "access/twophase.h"
#include "postmaster/backoff.h"
#include <pthread.h>
#include "utils/resscheduler.h"
#include "pgstat.h"
#include "executor/nodeFunctionscan.h"
#include "cdb/cdbfilerep.h"
#include "postmaster/primary_mirror_mode.h"

bool            assert_enabled;
void 
CdbProgramErrorHandler(int postgres_signal_arg)
{

    check_expected(postgres_signal_arg);

    mock();

}

void 
StandardHandlerForSigillSigsegvSigbus_OnMainThread(char *processName, int postgres_signal_arg)
{

    check_expected(processName);
    check_expected(postgres_signal_arg);
    optional_assignment(processName);
    mock();

}

void 
gp_set_thread_sigmasks(void)
{



    mock();

}

volatile bool   InterruptPending;
volatile bool   QueryCancelPending;
volatile bool   QueryCancelErrorless;
volatile bool   QueryCancelCleanup;
volatile bool   ProcDiePending;
volatile bool   ImmediateInterruptOK;
volatile int32  InterruptHoldoffCount;
volatile int32  CritSectionCount;
void 
ProcessInterrupts(void)
{



    mock();

}

bool            gp_enable_resqueue_priority;
pid_t           PostmasterPid;
bool            IsPostmasterEnvironment;
bool            IsUnderPostmaster;
bool            ExitOnAnyError;
char           *DataDir;
int             NBuffers;
int             MaxBackends;
int             MaxConnections;
int             MyProcPid;
pg_time_t       MyStartTime;
struct Port    *MyProcPort;
long            MyCancelKey;
int             MyPMChildSlot;
char            OutputFileName[];
char            my_exec_path[];
char            pkglib_path[];
int             gpperfmon_port;
char           *pljava_vmoptions;
char           *pljava_classpath;
int             pljava_statement_cache_size;
bool            pljava_debug;
bool            pljava_release_lingering_savepoints;
Oid             MyDatabaseId;
Oid             MyDatabaseTableSpace;
int             DateStyle;
int             DateOrder;
int             IntervalStyle;
bool            HasCTZSet;
int             CTimeZone;
bool            enableFsync;
bool            allowSystemTableModsDDL;
bool            allowSystemTableModsDML;
int             planner_work_mem;
int             work_mem;
int             max_work_mem;
int             maintenance_work_mem;
int             statement_mem;
int             max_statement_mem;
int             VacuumCostPageHit;
int             VacuumCostPageMiss;
int             VacuumCostPageDirty;
int             VacuumCostLimit;
int             VacuumCostDelay;
int             VacuumCostBalance;
bool            VacuumCostActive;
int             gp_vmem_protect_limit;
int             gp_vmem_protect_gang_cache_limit;
void 
check_stack_depth(void)
{



    mock();

}

char           *DatabasePath;
void 
SetDatabasePath(const char *path)
{

    check_expected(path);

    mock();

}

char           *
GetUserNameFromId(Oid roleid)
{

    check_expected(roleid);

    return (char *) mock();

}

Oid 
GetUserId(void)
{



    return (Oid) mock();

}

Oid 
GetOuterUserId(void)
{



    return (Oid) mock();

}

Oid 
GetSessionUserId(void)
{



    return (Oid) mock();

}

void 
SetSessionUserId(Oid userid, bool is_superuser)
{

    check_expected(userid);
    check_expected(is_superuser);

    mock();

}

Oid 
GetAuthenticatedUserId(void)
{



    return (Oid) mock();

}

bool 
IsAuthenticatedUserSuperUser(void)
{



    return (bool) mock();

}

void 
GetUserIdAndContext(Oid * userid, bool * sec_def_context)
{

    check_expected(userid);
    check_expected(sec_def_context);
    optional_assignment(userid);
    optional_assignment(sec_def_context);
    mock();

}

void 
SetUserIdAndContext(Oid userid, bool sec_def_context)
{

    check_expected(userid);
    check_expected(sec_def_context);

    mock();

}

bool 
InSecurityDefinerContext(void)
{



    return (bool) mock();

}

void 
InitializeSessionUserId(const char *rolename)
{

    check_expected(rolename);

    mock();

}

void 
InitializeSessionUserIdStandalone(void)
{



    mock();

}

void 
SetSessionAuthorization(Oid userid, bool is_superuser)
{

    check_expected(userid);
    check_expected(is_superuser);

    mock();

}

Oid 
GetCurrentRoleId(void)
{



    return (Oid) mock();

}

void 
SetCurrentRoleId(Oid roleid, bool is_superuser)
{

    check_expected(roleid);
    check_expected(is_superuser);

    mock();

}

void 
SetDataDir(const char *dir)
{

    check_expected(dir);

    mock();

}

void 
ChangeToDataDir(void)
{



    mock();

}

char           *
make_absolute_path(const char *path)
{

    check_expected(path);

    return (char *) mock();

}

bool 
superuser(void)
{



    return (bool) mock();

}

bool 
superuser_arg(Oid roleid)
{

    check_expected(roleid);

    return (bool) mock();

}

ProcessingMode  Mode;
bool 
InitPostgres(const char *in_dbname, Oid dboid, const char *username, char **out_dbname)
{

    check_expected(in_dbname);
    check_expected(dboid);
    check_expected(username);
    check_expected(out_dbname);
    optional_assignment(out_dbname);
    return (bool) mock();

}

void 
BaseInit(void)
{



    mock();

}

bool            IgnoreSystemIndexes;
bool            process_shared_preload_libraries_in_progress;
char           *shared_preload_libraries_string;
char           *local_preload_libraries_string;
void 
SetReindexProcessing(Oid heapOid, Oid indexOid)
{

    check_expected(heapOid);
    check_expected(indexOid);

    mock();

}

void 
ResetReindexProcessing(void)
{



    mock();

}

bool 
ReindexIsProcessingHeap(Oid heapOid)
{

    check_expected(heapOid);

    return (bool) mock();

}

bool 
ReindexIsProcessingIndex(Oid indexOid)
{

    check_expected(indexOid);

    return (bool) mock();

}

void 
CreateDataDirLockFile(bool amPostmaster)
{

    check_expected(amPostmaster);

    mock();

}

void 
CreateSocketLockFile(const char *socketfile, bool amPostmaster)
{

    check_expected(socketfile);
    check_expected(amPostmaster);

    mock();

}

void 
TouchSocketLockFile(void)
{



    mock();

}

void 
RecordSharedMemoryInLockFile(unsigned long id1, unsigned long id2)
{

    check_expected(id1);
    check_expected(id2);

    mock();

}

void 
ValidatePgVersion(const char *path)
{

    check_expected(path);

    mock();

}

void 
process_shared_preload_libraries(void)
{



    mock();

}

void 
process_local_preload_libraries(void)
{



    mock();

}

void 
pg_bindtextdomain(const char *domain)
{

    check_expected(domain);

    mock();

}

int64 
db_dir_size(const char *path)
{

    check_expected(path);

    return (int64) mock();

}
