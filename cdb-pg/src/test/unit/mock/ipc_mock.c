/*
 * Auto-generated Mocking Source
 * 
 * Source: storage/ipc.h
 * 
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/ipc.h"

bool            proc_exit_inprogress;
void 
proc_exit(int code)
{

    check_expected(code);

    mock();

}

void 
shmem_exit(int code)
{

    check_expected(code);

    mock();

}

void 
on_proc_exit(pg_on_exit_callback function, Datum arg)
{

    check_expected(function);
    check_expected(arg);

    mock();

}

void 
on_shmem_exit(pg_on_exit_callback function, Datum arg)
{

    check_expected(function);
    check_expected(arg);

    mock();

}

void 
cancel_shmem_exit(pg_on_exit_callback function, Datum arg)
{

    check_expected(function);
    check_expected(arg);

    mock();

}

void 
on_exit_reset(void)
{



    mock();

}

void 
proc_exit_prepare(int code)
{

    check_expected(code);

    mock();

}

shmem_startup_hook_type shmem_startup_hook;
bool            gp_simex_enable;
void 
CreateSharedMemoryAndSemaphores(bool makePrivate, int port)
{

    check_expected(makePrivate);
    check_expected(port);

    mock();

}
