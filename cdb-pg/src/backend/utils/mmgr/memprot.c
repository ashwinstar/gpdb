/* 
 * memprot.c
 *		Memory allocation under greenplum memory allocation.
 * 
 * Copyright(c) 2008, Greenplum Inc.
 * 
 * We wrap up calls to malloc/realloc/free with our own accounting
 * so that we will make sure a postgres process will not go beyond
 * its allowed quota
 */

#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/time.h>

#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif
#ifdef HAVE_SYS_SEM_H
#include <sys/sem.h>
#endif
#ifdef HAVE_KERNEL_OS_H
#include <kernel/OS.h>
#endif

#include "miscadmin.h"
#include "storage/pg_sema.h"
#include "utils/palloc.h"

#include "cdb/cdbvars.h"
#include "utils/debugbreak.h"
#include "utils/faultinjection.h" 

#ifndef HAVE_UNION_SEMUN
union semun
{
	int val;
	struct semid_ds *buf;
	unsigned short *array;
};
#endif

#ifndef USE_SYSV_SEMAPHORES
/*
 * Greenplum Physical Mem Protection is implemented using SysV
 * semaphore (using SEM_UNDO) to clean up, if the postgres process
 * dies.
 *
 * On a non-sysv system, make it no-op
 */
void GPMemoryProtectInit()
{
	elog(LOG, "GPDB on this platform does not use SysV Semeaphore.  Physical Mem Protection not implemented");
}
#ifdef USE_FAULT_INJECTION
int64 gp_mp_fault(int32 reason, int64 arg)
{
	elog(LOG, "GPDB does not support mem prot on this plat form.  Fault reason %d, arg" INT64_FORMAT ".", reason, arg);
	return -1;
}
#endif

#else

/* Helpers */
static int gpmemprot_down_sem(PGSemaphoreData *psem, int amount, bool undo);
static int gpmemprot_up_sem(PGSemaphoreData *psem, int amount, bool undo);
static int gpmemprot_peek_sem(PGSemaphoreData *psem);


/* Global physical mem counter */
static PGSemaphoreData gpsema_vmem_prot;
#ifdef USE_FAULT_INJECTION
static PGSemaphoreData gpsema_mp_fault;
#endif

/* By default, we will inc/dec counter, in MB. */
static int gpsema_vmem_prot_max;
static int gp_memprot_chunksize_shift = 20; 

/* total allocation in bytes */
static size_t gpproc_total_malloc;
static size_t gpproc_total_malloc_chunk; 

static bool gp_mp_inited = false;

static inline bool gp_memprot_enabled()
{
	if(!gp_mp_inited)
		return false;

#ifdef USE_FAULT_INJECTION
	return gpmemprot_peek_sem(&gpsema_mp_fault) != 0;
#else
	return gp_vmem_protect_limit != 0;
#endif
}

/* Initialization */
void GPMemoryProtectInit()
{
	/* 
	 * NOTES:
	 * 
	 * On most popular unix system, the max value of a semaphore is 65535.  With
	 * default 32767.  The impl. depends on the SEM_UNDO flag.  In order for the OS
	 * to keep track of the undo count, we cannot use the higher half.  So we set
	 * the max to 16 * 1024 here.  Each inc/dec of the semaphore corresponds to 
	 * chunksize of bytes memory allocation.  Chunk size default to 1MB. 
	 */

	Assert(gp_memprot_chunksize_shift == 20);

	gpsema_vmem_prot_max = gp_vmem_protect_limit;
	while(gpsema_vmem_prot_max > (16 * 1024))
	{
		gp_memprot_chunksize_shift++;
		gpsema_vmem_prot_max >>= 1;
	}

#ifdef USE_FAULT_INJECTION
	PGSemaphoreCreateInitVal(&gpsema_vmem_prot, gpsema_vmem_prot_max);
	PGSemaphoreCreateInitVal(&gpsema_mp_fault, gpsema_vmem_prot_max);
#else
	if(gpsema_vmem_prot_max != 0)
		PGSemaphoreCreateInitVal(&gpsema_vmem_prot, gpsema_vmem_prot_max);
#endif

	gpproc_total_malloc = 0;
	gpproc_total_malloc_chunk = 0;

	gp_mp_inited = true;
}

/* 
 * XXX Why we put the ereport here.
 * Theoretically, the ereport/elog family should NEVER allocate memory.
 * The mem should be reserved at startup time.  However, this is NOT 
 * true.  
 * 
 * So if we out of mem, failed to alloc, we ereprot.  Then in the error
 * handling, we may palloc again, which will fail again, then we will call
 * ereport again ... ...
 *
 * HACK: When we fail here, we give them 1 more chunk of quota.  Error
 * handling should be done with this 1 more chunk (1M, most likely).  
 * NOTE: If we have mem prot enabled, we should run into vmem limit before
 * we will really fail malloc.  Once given this extra chunk, malloc in 
 * error handling should be OK.  The 1 more chunk is not leaked from accounting
 * because after the call, the postgres process will be killed in a short
 * time anyway.
 *
 * FIX ERROR HANDLER!
 */
static void gp_failed_to_alloc()
{
	/* Give an extra chunk for error handling. */
	gpproc_total_malloc_chunk += 1;
	ereport(ERROR, (errcode(ERRCODE_GP_MEMPROT_KILL),
				errmsg("Memory allocation denied, kill all gangs")
				));
}

static void *gp_malloc_internal(size_t sz1, size_t sz2, bool ismalloc)
{
	size_t newsz_chunk;
	size_t sz = sz1;
	int need_chunk = 0;

	void *ret = NULL;

	if(!ismalloc)
		sz *= sz2;

	Assert(sz >=0 && sz <= 0x7fffffff);

	newsz_chunk = (gpproc_total_malloc + sz) >> gp_memprot_chunksize_shift;

	if(newsz_chunk > gpproc_total_malloc_chunk)
	{
		int mem_avail;
		int err_code = -1;

		need_chunk = newsz_chunk - gpproc_total_malloc_chunk; 
		mem_avail = gpmemprot_peek_sem(&gpsema_vmem_prot);

		if(mem_avail > need_chunk)
			err_code = gpmemprot_down_sem(&gpsema_vmem_prot, need_chunk, true);

		if(err_code != 0)
			gp_failed_to_alloc();
	}

	if(ismalloc)
		ret = malloc(sz);
	else
		ret = calloc(sz1, sz2);

	if(!ret)
	{
		/* Try my best to be honest with allocation */
		if(need_chunk > 0)
			gpmemprot_up_sem(&gpsema_vmem_prot, need_chunk, true);
		return NULL;
	}

	gpproc_total_malloc += sz;
	gpproc_total_malloc_chunk += need_chunk;

	return ret;
}

void *gp_malloc(size_t sz)
{
	void *ret;

	if(gp_mp_inited && gp_memprot_enabled())
		return gp_malloc_internal(sz, 0, true);

	ret = malloc(sz);
	if(ret)
		gpproc_total_malloc += sz;

	return ret;
}

void *gp_calloc(size_t sz1, size_t sz2)
{
	void *ret;

	if(gp_mp_inited && gp_memprot_enabled())
		return gp_malloc_internal(sz1, sz2, false);

	ret = calloc(sz1, sz2);
	if(ret)
		gpproc_total_malloc += sz1 * sz2;

	return ret;
}

void *gp_realloc(void *ptr, size_t sz, size_t newsz)
{
	size_t newsz_chunk;
	int need_chunk = 0;

	void *ret = NULL;

	if(gp_mp_inited && gp_memprot_enabled())
	{
		if(newsz > sz)
		{
			newsz_chunk = (gpproc_total_malloc - sz + newsz) >> gp_memprot_chunksize_shift;

			if(newsz_chunk > gpproc_total_malloc_chunk)
			{
				int mem_avail;
				int err_code = -1;

				need_chunk = newsz_chunk - gpproc_total_malloc_chunk; 
				mem_avail = gpmemprot_peek_sem(&gpsema_vmem_prot);

				if(mem_avail > need_chunk)
					err_code = gpmemprot_down_sem(&gpsema_vmem_prot, need_chunk, true);

				if(err_code != 0)
					gp_failed_to_alloc();
			}
		}

		ret = realloc(ptr, newsz);

		if(!ret)
		{
			/* Try my best to be honest with allocation */
			if(need_chunk > 0)
				gpmemprot_up_sem(&gpsema_vmem_prot, need_chunk, true);
			return NULL;
		}

		gpproc_total_malloc += (newsz - sz);
		gpproc_total_malloc_chunk += need_chunk;

		return ret;
	}
	else
	{
		ret = realloc(ptr, newsz);

		if(ret)
			gpproc_total_malloc += (newsz - sz);
		return ret;
	}

	/* Never reach here*/
	return NULL; 
}

void gp_free(void *ptr, size_t sz)
{
	free(ptr);
	gpproc_total_malloc -= sz;
}

int gpmemprot_up_sem(PGSemaphoreData *psem, int n, bool undo)
{
	struct sembuf sop;
	int err;

	sop.sem_op = n;
	sop.sem_flg = IPC_NOWAIT;
	if(undo)
		sop.sem_flg |= SEM_UNDO;
	sop.sem_num = psem->semNum; 

	do {
		err = semop(psem->semId, &sop, 1);
	} while (err < 0 && errno == EINTR);

	return err;
}

int gpmemprot_down_sem(PGSemaphoreData *psem, int n, bool undo)
{
	return gpmemprot_up_sem(psem, -n, undo);
}

int gpmemprot_peek_sem(PGSemaphoreData *psem)
{
	union semun s;
	int ret;

	ret = semctl(psem->semId, psem->semNum, GETVAL, s);
	if(ret < 0)
		elog(ERROR, "Greenplum memory protection failed to get memory counter");

	return ret;
}

#ifdef USE_FAULT_INJECTION
static inline int64 chunk_to_mb(int64 chunk)
{
	Assert(gp_memprot_chunksize_shift >= 20);
	return chunk << (gp_memprot_chunksize_shift - 20);
}
static inline int mb_to_chunk(int mb)
{
	Assert(gp_memprot_chunksize_shift >= 20);
	return mb >> (gp_memprot_chunksize_shift - 20);
}

int64 gp_mp_fault(int32 reason, int64 arg)
{
	switch(reason)
	{
		case GP_FAULT_USER_MP_CONFIG:
			return (int64) gp_vmem_protect_limit;
		case GP_FAULT_USER_MP_ALLOC:
			return (int64) (gpproc_total_malloc >> 20);
		case GP_FAULT_USER_MP_HIGHWM:
			return chunk_to_mb(gpproc_total_malloc_chunk); 
		case GP_FAULT_SEG_AVAILABLE:
			{
				int avail = gpmemprot_peek_sem(&gpsema_vmem_prot);
				return chunk_to_mb(avail);
			}
		case GP_FAULT_SEG_SET_VMEMMAX:
			{
				int oldval; 
				int change;  
				int err;

				int newval = mb_to_chunk(arg);

				if(newval < 0 || newval > 16 * 1024)
					elog(ERROR, "GP MP Fault failed to change vmem limit: invalid value");
				
				oldval = gpmemprot_peek_sem(&gpsema_mp_fault);
				change = newval - oldval;
				err = gpmemprot_up_sem(&gpsema_vmem_prot, change, false);

				if(err != 0)
					elog(ERROR, "GP MP Fault failed to change vmem limit, cannot update vmem");

				err = gpmemprot_up_sem(&gpsema_mp_fault, change, false);
				if(err != 0)
					elog(ERROR, "GP MP Fault failed to change vmem limit, cannot do bookkeeping.");
				return arg;
			}
		case GP_FAULT_SEG_GET_VMEMMAX:
			return chunk_to_mb(gpmemprot_peek_sem(&gpsema_mp_fault));
		default:
			elog(ERROR, "GP MP Fault Invalid fault code");
	}

	return -1;
}
#endif /* Fault Inj */
#endif /* SysV Semaphore */
