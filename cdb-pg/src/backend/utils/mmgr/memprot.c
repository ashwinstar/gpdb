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

#include "miscadmin.h"
#include "postmaster/memprotserver.h"

#include "cdb/cdbvars.h"
#include "utils/debugbreak.h"

static bool memprot_active = false;
static size_t gp_malloc_total = 0; 		/* Total malloced bytes */
static size_t gp_malloc_total_req = 0; 	/* Total requested bytes from memprot server */
static size_t gp_malloc_total_grant = 0;
static size_t gp_malloc_highmark = 0;	/* High water mark. */

void MarkMemProtActive()
{
	Assert(!memprot_active);
	/* Only mark memprot active when under postmaster so that
	 * init db will work.
	 */
	if(IsUnderPostmaster)
	{
		memprot_active = true;
		gp_malloc_total_req = 0; 
		gp_malloc_total_grant = 0; 
		gp_malloc_highmark = gp_malloc_total; 
	}
}

bool IsMemProtActive()
{
	return memprot_active;
}

/* Cleanup when postgres process exits (if it had a chance) */
void MemProtCleanup()
{
	if(IsMemProtActive())
	{
		bool reqOK;
		MemProtectRequest m_req;
		MemProtectResponse m_response;

		m_req.command = MemProt_Release;
		m_req.pid = MyProcPid;
		m_req.req_size = gp_malloc_total_grant; 

		reqOK = MemProtect_Do(&m_req, &m_response);
		if(!reqOK)
			elog(ERROR, "Memory release request failed");

		if(m_response.command != MemProt_Grant)
			elog(FATAL, "Memprot denied release, reset all");

		gp_malloc_total_req = 0; 
		gp_malloc_total_grant = 0;
		gp_malloc_highmark -= m_response.grant_size; 

		/* Mark mem protection in active */
		memprot_active = false;
	}
}

/* 
 * Request sz bytes from memprotect server.  Here we just ask for 
 * quota and do not call malloc.
 */
static bool gp_malloc_request(size_t sz)
{
	/* Do not request from mprot server for initdb.
	 * Test if mem prot is active before contacting server.
	 */
	if(IsMemProtActive()) 
	{
		bool reqOK;
		MemProtectRequest m_req;
		MemProtectResponse m_response;

		m_req.command = MemProt_Request;
		m_req.pid = MyProcPid;
		m_req.req_size = sz;

		reqOK = MemProtect_Do(&m_req, &m_response);
		if(!reqOK)
			elog(ERROR, "Memory allocation request failed");

		if(m_response.command != MemProt_Grant)
			ereport(ERROR, (errcode(ERRCODE_GP_MEMPROT_KILL),
						errmsg("Memory allocation request denied, kill all gangs")));

		Assert(m_response.grant_size >= m_req.req_size);
		gp_malloc_total_req += sz;
		gp_malloc_total_grant += m_response.grant_size;
		gp_malloc_highmark += m_response.grant_size;
	}
	else
		gp_malloc_highmark += sz;

	return true;
}


/* Helper.  Common code for malloc, calloc */
static void *gp_malloc_internal(size_t sz) 
{
	size_t newsz_MB;
	size_t oldsz_MB;
	void *ret;

	bool reqOK;

	Assert(sz > 0 && sz <= 0x3fffffff);
	AssertImply(IsMemProtActive(), gp_malloc_total <= gp_malloc_highmark + (1 << 20));

	newsz_MB = (gp_malloc_total + sz) >> 20;
	oldsz_MB = (gp_malloc_highmark) >> 20;

	if(newsz_MB <= oldsz_MB) 
	{
		ret = malloc(sz);
		if(ret)
			gp_malloc_total += sz;

		return ret;
	}

	reqOK = gp_malloc_request(sz);
	Assert(reqOK);

	ret = malloc(sz);
	if(ret)
		gp_malloc_total += sz;

	return ret;
}

/* 
 * Using write_stderr instead of elog(LOG, ...).
 * Still, we have some palloc calles inside elog(...) stuff.  
 * If gp_malloc fails, calling elog is doomed in a loop to overflow
 * stack.
 * 
 * XXX Should fix elog.
 */
#ifdef DEBUG_PRINT_GP_MALLOC
void *gp_malloc_impl(size_t sz, const char*fn, int line) 
#else
void *gp_malloc(size_t sz) 
#endif
{
	void *ptr = gp_malloc_internal(sz);
#ifdef DEBUG_PRINT_GP_MALLOC
	write_stderr("GP_MALLOC, %s, %d, %d, %p, 0, %d, 0\n", fn, line, MyProcPid, ptr, (int) sz);
#endif
	return ptr;
}

#ifdef DEBUG_PRINT_GP_MALLOC
void *gp_calloc_impl(size_t sz1, size_t sz2, const char *fn, int line)
#else
void *gp_calloc(size_t sz1, size_t sz2)
#endif
{
	void *ptr = gp_malloc_internal(sz1 * sz2);
#ifdef DEBUG_PRINT_GP_MALLOC
	write_stderr("GP_CALLOC, %s, %d, %d, %p, 0, %d, 0\n", fn, line, MyProcPid, ptr, (int) (sz1 * sz2));
#endif
	return ptr;
}

#ifdef DEBUG_PRINT_GP_MALLOC
void gp_free_impl(void *ptr, size_t sz, const char *fn, int line)
#else
void gp_free(void *ptr, size_t sz) 
#endif
{
	Assert(sz > 0 && sz <= 0x3fffffff);
	Assert(gp_malloc_total >= sz);
	gp_malloc_total -= sz;

#ifdef DEBUG_PRINT_GP_MALLOC
	write_stderr("GP_FREE, %s, %d, %d, 0, %p, 0, %d\n", fn, line, MyProcPid, ptr, (int) (sz));
#endif
	free(ptr);
}

#ifdef DEBUG_PRINT_GP_MALLOC
void *gp_realloc_impl(void *ptr, size_t sz1, size_t sz2, const char* fn, int line)
#else
void *gp_realloc(void *ptr, size_t sz1, size_t sz2) 
#endif
{
	size_t newsz_MB;
	size_t oldsz_MB;
	size_t sz;

	void *ret;
	bool reqOK;

	Assert(sz1 > 0 && sz1 <= 0x3fffffff);
	Assert(sz2 > 0 && sz2 <= 0x3fffffff);
	AssertImply(IsMemProtActive(), gp_malloc_total <= gp_malloc_highmark + (1 << 20));

	if(sz1 >= sz2)
	{
		gp_malloc_total -= (sz1 - sz2); 
		return ptr;
	}

	sz = sz2 - sz1;
	newsz_MB = (gp_malloc_total + sz) >> 20;
	oldsz_MB = (gp_malloc_highmark) >> 20;

	if(newsz_MB <= oldsz_MB) 
	{
		ret = realloc(ptr, sz2);
		if(ret)
			gp_malloc_total += sz;

#ifdef DEBUG_PRINT_GP_MALLOC
		write_stderr("GR_REALLOC, %s, %d, %d, %p, %p, %d, %d\n", fn, line, MyProcPid, ret, ptr, (int) sz2, (int) sz1);
#endif
		return ret;
	}

	reqOK = gp_malloc_request(sz);
	Assert(reqOK);
	
	ret = realloc(ptr, sz2);
	if(ret)
		gp_malloc_total += sz;

#ifdef DEBUG_PRINT_GP_MALLOC
	write_stderr("GR_REALLOC, %s, %d, %d, %p, %p, %d, %d\n", fn, line, MyProcPid, ret, ptr, (int) sz2, (int) sz1);
#endif
	return ret;
}
