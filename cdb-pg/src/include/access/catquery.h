/*-------------------------------------------------------------------------
 *
 * catquery.h
 *	  catalog query
 *
 *
 * Copyright (c) 2011, 2012 Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATQUERY_H
#define CATQUERY_H

#include "access/genam.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "nodes/primnodes.h"
#include "storage/lock.h"
#include "utils/relcache.h"

typedef struct cqListData
{
	const char* caqlStr;		/* caql string */
	int			numkeys;
	int			maxkeys;
	const char* filename;		/* FILE and LINE macros */
	int			lineno;
	Datum		cqlKeys[5];		/* key array */
	Oid			relationId;
} cq_list;

typedef struct cqContextData
{
	bool		cq_free;		/* if true, free this ctx at endscan */
	Relation	cq_heap_rel;	/* catalog being scanned */
	TupleDesc	cq_tupdesc;		/* tuple descriptor */
	SysScanDesc cq_sysScan;		/* heap or index scan */		
	bool		cq_externrel;	/* heap rel external to caql */
	bool		cq_setsnapshot;	/* use cq_snapshot (else default) */
	Snapshot	cq_snapshot;	/* snapshot to see */
	bool		cq_setlockmode;	/* use cq_lockmode (else default) */
	LOCKMODE	cq_lockmode;	/* locking mode */
	bool		cq_EOF;			/* true if hit end of fetch */

	bool		cq_setidxOK;	/* set the indexOK mode */
	bool		cq_useidxOK;	/* use supplied indexOK mode) */

	/* 	these attributes control syscache vs normal heap/index
	 * 	scan. If setsyscache is set, then usesyscache is the
	 * 	user-setting, else catquery chooses.  If usesyscache is set
	 * 	then sysScan is NULL, and cq_cacheId is the
	 * 	SysCacheIdentifier.
	 */
	bool		cq_setsyscache;	/* use syscache (else heap/index scan) */
	bool		cq_usesyscache;	/* use syscache (internal) */
	int			cq_cacheId; 	/* cache identifier */
	int			cq_cacheNumKeys;/* number of keys */
	Datum	   *cq_cacheKeys;	/* array of keys */
	HeapTuple   cq_lasttup;		/* last tuple fetched (for ReleaseSysCache) */

} cqContext;

/* count (and optionally delete) */
int			 caql_getcount(cqContext *pCtx, cq_list *pcql);

/* return the first tuple 
   (and set a flag if the scan finds a second match) 
*/
HeapTuple	 caql_getfirst_only(cqContext *pCtx, 
								bool *pbOnly,
								cq_list *pcql);
#define		 caql_getfirst(pCtx, pcql) caql_getfirst_only(pCtx, NULL, pcql)

/* return the specified oid column of the first tuple 
   (and set a flag if the scan finds a second match) 
*/
Oid			 caql_getoid_only(cqContext *pCtx, 
							  bool *pbOnly,
							  cq_list *pcql);
Oid			 caql_getoid_plus(cqContext *pCtx0, int *pFetchcount,
							  bool *pbIsNull, cq_list *pcql);
#define		 caql_getoid(pCtx, pcql) caql_getoid_plus(pCtx, NULL, NULL, pcql)

cqContext	*caql_beginscan(cqContext *pCtx, cq_list *pcql);
HeapTuple	 caql_getnext(cqContext *pCtx);
HeapTuple	 caql_getprev(cqContext *pCtx);
/* XXX XXX: endscan must specify if hold or release locks */
void		 caql_endscan(cqContext *pCtx);

/* during beginscan/endscan iteration, delete current tuple */
void		 caql_delete_current(cqContext *pCtx);
/* during beginscan/endscan iteration, update current tuple */
void		 caql_update_current(cqContext *pCtx, HeapTuple tup);

/* return the specified Name or Text column of the first tuple 
   (and set the fetchcount or isnull if specified)
*/
char		*caql_getcstring_plus(cqContext *pCtx0, int *pFetchcount,
								  bool *pbIsNull, cq_list *pcql);
#define		 caql_getcstring(pCtx, pcql) \
		caql_getcstring_plus(pCtx, NULL, NULL, pcql)

cq_list *cql1(const char* caqlStr, const char* filename, int lineno, ...);

/* caql context modification functions 
 *
 * cqclr and caql_addrel() are ok, but the others are generally used
 * to support legacy code requirements, and may be deprecated in
 * future releases
 */
cqContext	*caql_addrel(cqContext *pCtx, Relation rel);		/*  */
cqContext	*caql_indexOK(cqContext *pCtx, bool bindexOK);		/*  */
cqContext	*caql_lockmode(cqContext *pCtx, LOCKMODE lm);		/*  */
cqContext	*caql_snapshot(cqContext *pCtx, Snapshot ss);		/*  */
cqContext	*caql_syscache(cqContext *pCtx, bool bUseCache);	/*  */

cqContext	*cqclr(cqContext	 *pCtx);						/*  */
#define	cqClearContext(pcqContext) MemSet(pcqContext, 0, sizeof(cqContext))

/* CAQL prototype: expand to nothing */
#define cql0(x, ...) if (0) {} else 

/* ifdef gnuc ! */
#define cql(x, ...) cql1(x, __FILE__, __LINE__, __VA_ARGS__)

#endif   /* CATQUERY_H */
