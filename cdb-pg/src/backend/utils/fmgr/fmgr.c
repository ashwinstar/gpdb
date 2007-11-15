/*-------------------------------------------------------------------------
 *
 * fmgr.c
 *	  The Postgres function manager.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/fmgr/fmgr.c,v 1.102.2.1 2007/07/31 15:49:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tuptoaster.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "executor/functions.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"
#include "utils/fmgrtab.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbvars.h"

/*
 * Declaration for old-style function pointer type.  This is now used only
 * in fmgr_oldstyle() and is no longer exported.
 *
 * The m68k SVR4 ABI defines that pointers are returned in %a0 instead of
 * %d0. So if a function pointer is declared to return a pointer, the
 * compiler may look only into %a0, but if the called function was declared
 * to return an integer type, it puts its value only into %d0. So the
 * caller doesn't pick up the correct return value. The solution is to
 * declare the function pointer to return int, so the compiler picks up the
 * return value from %d0. (Functions returning pointers put their value
 * *additionally* into %d0 for compatibility.) The price is that there are
 * some warnings about int->pointer conversions...
 */
#if (defined(__mc68000__) || (defined(__m68k__))) && defined(__ELF__)
typedef int32 (*func_ptr) ();
#else
typedef char *(*func_ptr) ();
#endif

/*
 * Hashtable for fast lookup of external C functions
 */
typedef struct
{
	/* fn_oid is the hash key and so must be first! */
	Oid			fn_oid;			/* OID of an external C function */
	TransactionId fn_xmin;		/* for checking up-to-dateness */
	ItemPointerData	fn_tid;
	PGFunction	user_fn;		/* the function's address */
	const Pg_finfo_record *inforec;		/* address of its info record */
} CFuncHashTabEntry;

static HTAB *CFuncHash = NULL;


static void fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
					   bool ignore_security);
static void fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static void fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static CFuncHashTabEntry *lookup_C_func(HeapTuple procedureTuple);
static void record_C_func(HeapTuple procedureTuple,
			  PGFunction user_fn, const Pg_finfo_record *inforec);
static Datum fmgr_security_definer(PG_FUNCTION_ARGS);


/*
 * Lookup routines for builtin-function table.	We can search by either Oid
 * or name, but search by Oid is much faster.
 */

static const FmgrBuiltin *
fmgr_isbuiltin(Oid id)
{
	int			low = 0;
	int			high = fmgr_nbuiltins - 1;

	/*
	 * Loop invariant: low is the first index that could contain target entry,
	 * and high is the last index that could contain it.
	 */
	while (low <= high)
	{
		int			i = (high + low) / 2;
		const FmgrBuiltin *ptr = &fmgr_builtins[i];

		if (id == ptr->foid)
			return ptr;
		else if (id > ptr->foid)
			low = i + 1;
		else
			high = i - 1;
	}
	return NULL;
}

/*
 * Lookup a builtin by name.  Note there can be more than one entry in
 * the array with the same name, but they should all point to the same
 * routine.
 */
static const FmgrBuiltin *
fmgr_lookupByName(const char *name)
{
	int			i;

	for (i = 0; i < fmgr_nbuiltins; i++)
	{
		if (strcmp(name, fmgr_builtins[i].funcName) == 0)
			return fmgr_builtins + i;
	}
	return NULL;
}

/*
 * This routine fills a FmgrInfo struct, given the OID
 * of the function to be called.
 *
 * The caller's CurrentMemoryContext is used as the fn_mcxt of the info
 * struct; this means that any subsidiary data attached to the info struct
 * (either by fmgr_info itself, or later on by a function call handler)
 * will be allocated in that context.  The caller must ensure that this
 * context is at least as long-lived as the info struct itself.  This is
 * not a problem in typical cases where the info struct is on the stack or
 * in freshly-palloc'd space.  However, if one intends to store an info
 * struct in a long-lived table, it's better to use fmgr_info_cxt.
 */
void
fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt(functionId, finfo, CurrentMemoryContext);
}

/*
 * Fill a FmgrInfo struct, specifying a memory context in which its
 * subsidiary data should go.
 */
void
fmgr_info_cxt(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt)
{
	fmgr_info_cxt_security(functionId, finfo, mcxt, false);
}

/*
 * This one does the actual work.  ignore_security is ordinarily false
 * but is set to true by fmgr_security_definer to avoid infinite
 * recursive lookups.
 */
static void
fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
					   bool ignore_security)
{
	const FmgrBuiltin *fbp;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	Datum		prosrcdatum;
	bool		isnull;
	char	   *prosrc;

	/*
	 * fn_oid *must* be filled in last.  Some code assumes that if fn_oid is
	 * valid, the whole struct is valid.  Some FmgrInfo struct's do survive
	 * elogs.
	 */
	finfo->fn_oid = InvalidOid;
	finfo->fn_extra = NULL;
	finfo->fn_mcxt = mcxt;
	finfo->fn_expr = NULL;		/* caller may set this later */

	if ((fbp = fmgr_isbuiltin(functionId)) != NULL)
	{
		/*
		 * Fast path for builtin functions: don't bother consulting pg_proc
		 */
		finfo->fn_nargs = fbp->nargs;
		finfo->fn_strict = fbp->strict;
		finfo->fn_retset = fbp->retset;
		finfo->fn_addr = fbp->func;
		finfo->fn_oid = functionId;
		
		if (Gp_role == GP_ROLE_EXECUTE && !Gp_is_primary)
		{
			/*
			 * These built-in functions don't work if we are on a mirror, since
			 * they won't give the same result as the primary gets.
			 * 
			 * Skip the fastpath return, so they will be detected below.
			 * 
			 */
			if (functionId != 274 /* timeofday */ &&
				functionId != 1574 /* nextval */ &&
				functionId != 1575 /* currval */ &&
				functionId != 1576 /* setval */ &&
				functionId != 1598 /* random */ &&
				functionId != 1765 /* setval */ &&
				functionId != 2559 /* lastval */
				)
			return;
		}
		else
			return;
	}

	/* Otherwise we need the pg_proc entry */
	procedureTuple = SearchSysCache(PROCOID,
									ObjectIdGetDatum(functionId),
									0, 0, 0);
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	finfo->fn_nargs = procedureStruct->pronargs;
	finfo->fn_strict = procedureStruct->proisstrict;
	finfo->fn_retset = procedureStruct->proretset;
	
	if (Gp_role == GP_ROLE_EXECUTE && !Gp_is_primary && procedureStruct->provolatile != PROVOLATILE_IMMUTABLE)
	{
		if (strstr(NameStr(procedureStruct->proname),"_validator")!=0 && 
			strstr(NameStr(procedureStruct->proname),"_call_handler")!=0)
		{
			if (procedureStruct->provolatile == PROVOLATILE_VOLATILE)
				elog(ERROR,"Volatile function %s cannot be used in this statement when mirroring is active",NameStr(procedureStruct->proname));	
			if (procedureStruct->provolatile == PROVOLATILE_STABLE)
				elog(ERROR,"Stable function %s cannot be used in this statement when mirroring is active",NameStr(procedureStruct->proname));	
				
		}
	}

	if (procedureStruct->prosecdef && !ignore_security)
	{
		finfo->fn_addr = fmgr_security_definer;
		finfo->fn_oid = functionId;
		ReleaseSysCache(procedureTuple);
		return;
	}

	switch (procedureStruct->prolang)
	{
		case INTERNALlanguageId:

			/*
			 * For an ordinary builtin function, we should never get here
			 * because the isbuiltin() search above will have succeeded.
			 * However, if the user has done a CREATE FUNCTION to create an
			 * alias for a builtin function, we can end up here.  In that case
			 * we have to look up the function by name.  The name of the
			 * internal function is stored in prosrc (it doesn't have to be
			 * the same as the name of the alias!)
			 */
			prosrcdatum = SysCacheGetAttr(PROCOID, procedureTuple,
										  Anum_pg_proc_prosrc, &isnull);
			if (isnull)
				elog(ERROR, "null prosrc");
			prosrc = DatumGetCString(DirectFunctionCall1(textout,
														 prosrcdatum));
			fbp = fmgr_lookupByName(prosrc);
			if (fbp == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("internal function \"%s\" is not in internal lookup table",
								prosrc)));
			pfree(prosrc);
			/* Should we check that nargs, strict, retset match the table? */
			finfo->fn_addr = fbp->func;
			break;

		case ClanguageId:
			fmgr_info_C_lang(functionId, finfo, procedureTuple);
			break;

		case SQLlanguageId:
			finfo->fn_addr = fmgr_sql;
			break;

		default:
			fmgr_info_other_lang(functionId, finfo, procedureTuple);
			break;
	}

	finfo->fn_oid = functionId;
	ReleaseSysCache(procedureTuple);
}

/*
 * Special fmgr_info processing for C-language functions.  Note that
 * finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	CFuncHashTabEntry *hashentry;
	PGFunction	user_fn;
	const Pg_finfo_record *inforec;
	bool		isnull;

	/*
	 * See if we have the function address cached already
	 */
	hashentry = lookup_C_func(procedureTuple);
	if (hashentry)
	{
		user_fn = hashentry->user_fn;
		inforec = hashentry->inforec;
	}
	else
	{
		Datum		prosrcattr,
					probinattr;
		char	   *prosrcstring,
				   *probinstring;
		void	   *libraryhandle;

		/*
		 * Get prosrc and probin strings (link symbol and library filename)
		 */
		prosrcattr = SysCacheGetAttr(PROCOID, procedureTuple,
									 Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc for function %u", functionId);
		prosrcstring = DatumGetCString(DirectFunctionCall1(textout,
														   prosrcattr));

		probinattr = SysCacheGetAttr(PROCOID, procedureTuple,
									 Anum_pg_proc_probin, &isnull);
		if (isnull)
			elog(ERROR, "null probin for function %u", functionId);
		probinstring = DatumGetCString(DirectFunctionCall1(textout,
														   probinattr));

		/* Look up the function itself */
		user_fn = load_external_function(probinstring, prosrcstring, true,
										 &libraryhandle);

		/* Get the function information record (real or default) */
		inforec = fetch_finfo_record(libraryhandle, prosrcstring);

		/* Cache the addresses for later calls */
		record_C_func(procedureTuple, user_fn, inforec);

		pfree(prosrcstring);
		pfree(probinstring);
	}

	switch (inforec->api_version)
	{
		case 0:
			elog(ERROR, "Old style C functions (API version 0) are no longer supported by Greenplum");
			break;
		case 1:
			/* New style: call directly */
			finfo->fn_addr = user_fn;
			break;
		default:
			/* Shouldn't get here if fetch_finfo_record did its job */
			elog(ERROR, "unrecognized function API version: %d",
				 inforec->api_version);
			break;
	}
}

/*
 * Special fmgr_info processing for other-language functions.  Note
 * that finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	Oid			language = procedureStruct->prolang;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	FmgrInfo	plfinfo;

	languageTuple = SearchSysCache(LANGOID,
								   ObjectIdGetDatum(language),
								   0, 0, 0);
	if (!HeapTupleIsValid(languageTuple))
		elog(ERROR, "cache lookup failed for language %u", language);
	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

	fmgr_info(languageStruct->lanplcallfoid, &plfinfo);
	finfo->fn_addr = plfinfo.fn_addr;

	/*
	 * If lookup of the PL handler function produced nonnull fn_extra,
	 * complain --- it must be an oldstyle function! We no longer support
	 * oldstyle PL handlers.
	 */
	if (plfinfo.fn_extra != NULL)
		elog(ERROR, "language %u has old-style handler", language);

	ReleaseSysCache(languageTuple);
}

/*
 * Fetch and validate the information record for the given external function.
 * The function is specified by a handle for the containing library
 * (obtained from load_external_function) as well as the function name.
 *
 * If no info function exists for the given name, it is not an error.
 * Instead we return a default info record for a version-0 function.
 * We want to raise an error here only if the info function returns
 * something bogus.
 *
 * This function is broken out of fmgr_info_C_lang so that fmgr_c_validator
 * can validate the information record for a function not yet entered into
 * pg_proc.
 */
const Pg_finfo_record *
fetch_finfo_record(void *filehandle, char *funcname)
{
	char	   *infofuncname;
	PGFInfoFunction infofunc;
	const Pg_finfo_record *inforec;
	static Pg_finfo_record default_inforec = {0};

	/* Compute name of info func */
	infofuncname = (char *) palloc(strlen(funcname) + 10);
	strcpy(infofuncname, "pg_finfo_");
	strcat(infofuncname, funcname);

	/* Try to look up the info function */
	infofunc = (PGFInfoFunction) lookup_external_function(filehandle,
														  infofuncname);
	if (infofunc == NULL)
	{
		/* Not found --- assume version 0 */
		pfree(infofuncname);
		return &default_inforec;
	}

	/* Found, so call it */
	inforec = (*infofunc) ();

	/* Validate result as best we can */
	if (inforec == NULL)
		elog(ERROR, "null result from info function \"%s\"", infofuncname);
	switch (inforec->api_version)
	{
		case 0:
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("Old style C function (API version 0) are no longer supported by Greenplum")
				       ));
			break;
		case 1:
			/* OK, no additional fields to validate */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized API version %d reported by info function \"%s\"",
							inforec->api_version, infofuncname)));
			break;
	}

	pfree(infofuncname);
	return inforec;
}


/*-------------------------------------------------------------------------
 *		Routines for caching lookup information for external C functions.
 *
 * The routines in dfmgr.c are relatively slow, so we try to avoid running
 * them more than once per external function per session.  We use a hash table
 * with the function OID as the lookup key.
 *-------------------------------------------------------------------------
 */

/*
 * lookup_C_func: try to find a C function in the hash table
 *
 * If an entry exists and is up to date, return it; else return NULL
 */
static CFuncHashTabEntry *
lookup_C_func(HeapTuple procedureTuple)
{
	Oid			fn_oid = HeapTupleGetOid(procedureTuple);
	CFuncHashTabEntry *entry;

	if (CFuncHash == NULL)
		return NULL;			/* no table yet */
	entry = (CFuncHashTabEntry *)
		hash_search(CFuncHash,
					&fn_oid,
					HASH_FIND,
					NULL);
	if (entry == NULL)
		return NULL;			/* no such entry */
	if (entry->fn_xmin == HeapTupleHeaderGetXmin(procedureTuple->t_data) &&
		ItemPointerEquals(&entry->fn_tid, &procedureTuple->t_self))
		return entry;			/* OK */
	return NULL;				/* entry is out of date */
}

/*
 * record_C_func: enter (or update) info about a C function in the hash table
 */
static void
record_C_func(HeapTuple procedureTuple,
			  PGFunction user_fn, const Pg_finfo_record *inforec)
{
	Oid			fn_oid = HeapTupleGetOid(procedureTuple);
	CFuncHashTabEntry *entry;
	bool		found;

	/* Create the hash table if it doesn't exist yet */
	if (CFuncHash == NULL)
	{
		HASHCTL		hash_ctl;

		MemSet(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(CFuncHashTabEntry);
		hash_ctl.hash = oid_hash;
		CFuncHash = hash_create("CFuncHash",
								100,
								&hash_ctl,
								HASH_ELEM | HASH_FUNCTION);
	}

	entry = (CFuncHashTabEntry *)
		hash_search(CFuncHash,
					&fn_oid,
					HASH_ENTER,
					&found);
	/* OID is already filled in */
	entry->fn_xmin = HeapTupleHeaderGetXmin(procedureTuple->t_data);
	entry->fn_tid = procedureTuple->t_self;
	entry->user_fn = user_fn;
	entry->inforec = inforec;
}

/*
 * clear_external_function_hash: remove entries for a library being closed
 *
 * Presently we just zap the entire hash table, but later it might be worth
 * the effort to remove only the entries associated with the given handle.
 */
void
clear_external_function_hash(void *filehandle)
{
	if (CFuncHash)
		hash_destroy(CFuncHash);
	CFuncHash = NULL;
}


/*
 * Copy an FmgrInfo struct
 *
 * This is inherently somewhat bogus since we can't reliably duplicate
 * language-dependent subsidiary info.	We cheat by zeroing fn_extra,
 * instead, meaning that subsidiary info will have to be recomputed.
 */
void
fmgr_info_copy(FmgrInfo *dstinfo, FmgrInfo *srcinfo,
			   MemoryContext destcxt)
{
	memcpy(dstinfo, srcinfo, sizeof(FmgrInfo));
	dstinfo->fn_mcxt = destcxt;
	dstinfo->fn_extra = NULL;
}


/*
 * Specialized lookup routine for fmgr_internal_validator: given the alleged
 * name of an internal function, return the OID of the function.
 * If the name is not recognized, return InvalidOid.
 */
Oid
fmgr_internal_function(const char *proname)
{
	const FmgrBuiltin *fbp = fmgr_lookupByName(proname);

	if (fbp == NULL)
		return InvalidOid;
	return fbp->foid;
}

/*
 * Support for security definer functions
 */

struct fmgr_security_definer_cache
{
	FmgrInfo	flinfo;
	Oid			userid;
};

/*
 * Function handler for security definer functions.  We extract the
 * OID of the actual function and do a fmgr lookup again.  Then we
 * look up the owner of the function and cache both the fmgr info and
 * the owner ID.  During the call we temporarily replace the flinfo
 * with the cached/looked-up one, while keeping the outer fcinfo
 * (which contains all the actual arguments, etc.) intact.
 */
static Datum
fmgr_security_definer(PG_FUNCTION_ARGS)
{
	Datum		result;
	FmgrInfo   *save_flinfo;
	struct fmgr_security_definer_cache *volatile fcache;
	Oid			save_userid;
	HeapTuple	tuple;

	if (!fcinfo->flinfo->fn_extra)
	{
		fcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
										sizeof(*fcache));

		fmgr_info_cxt_security(fcinfo->flinfo->fn_oid, &fcache->flinfo,
							   fcinfo->flinfo->fn_mcxt, true);
		fcache->flinfo.fn_expr = fcinfo->flinfo->fn_expr;

		tuple = SearchSysCache(PROCOID,
							   ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u",
				 fcinfo->flinfo->fn_oid);
		fcache->userid = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;
		ReleaseSysCache(tuple);

		fcinfo->flinfo->fn_extra = fcache;
	}
	else
		fcache = fcinfo->flinfo->fn_extra;

	save_flinfo = fcinfo->flinfo;
	save_userid = GetUserId();

	PG_TRY();
	{
		fcinfo->flinfo = &fcache->flinfo;
		SetUserId(fcache->userid);

		result = FunctionCallInvoke(fcinfo);
	}
	PG_CATCH();
	{
		fcinfo->flinfo = save_flinfo;
		SetUserId(save_userid);
		PG_RE_THROW();
	}
	PG_END_TRY();

	fcinfo->flinfo = save_flinfo;
	SetUserId(save_userid);

	return result;
}


/*-------------------------------------------------------------------------
 *		Support routines for callers of fmgr-compatible functions
 *-------------------------------------------------------------------------
 */
/*
 * These are for invocation of a specifically named function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.	Also, the function cannot be one that needs to
 * look at FmgrInfo, since there won't be any.
 */
Datum
DirectFunctionCall1(PGFunction func, Datum arg1)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 1, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.argnull[0] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall2(PGFunction func, Datum arg1, Datum arg2)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 2, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall3(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 3, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall4(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 4, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall5(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 5, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall6(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 6, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall7(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 7, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall8(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7, Datum arg8)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 8, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;
	fcinfo.argnull[7] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall9(PGFunction func, Datum arg1, Datum arg2,
					Datum arg3, Datum arg4, Datum arg5,
					Datum arg6, Datum arg7, Datum arg8,
					Datum arg9)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, NULL, 9, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.arg[8] = arg9;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;
	fcinfo.argnull[7] = false;
	fcinfo.argnull[8] = false;

	result = (*func) (&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}


/*
 * These are for invocation of a previously-looked-up function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.
 */
Datum
FunctionCall1(FmgrInfo *flinfo, Datum arg1)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 1, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.argnull[0] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall2(FmgrInfo *flinfo, Datum arg1, Datum arg2)
{
	/*
	 * XXX if you change this routine, see also the inlined version in
	 * utils/sort/tuplesort.c!
	 */
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 2, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall3(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 3, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall4(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 4, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall5(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 5, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall6(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 6, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall7(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 7, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall8(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7, Datum arg8)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 8, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;
	fcinfo.argnull[7] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

Datum
FunctionCall9(FmgrInfo *flinfo, Datum arg1, Datum arg2,
			  Datum arg3, Datum arg4, Datum arg5,
			  Datum arg6, Datum arg7, Datum arg8,
			  Datum arg9)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 9, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.arg[8] = arg9;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;
	fcinfo.argnull[7] = false;
	fcinfo.argnull[8] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}


/*
 * These are for invocation of a function identified by OID with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.	These are essentially fmgr_info() followed
 * by FunctionCallN().	If the same function is to be invoked repeatedly,
 * do the fmgr_info() once and then use FunctionCallN().
 */
Datum
OidFunctionCall1(Oid functionId, Datum arg1)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 1, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.argnull[0] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall2(Oid functionId, Datum arg1, Datum arg2)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 2, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall3(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 3, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall4(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 4, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall5(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 5, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall6(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 6, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall7(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 7, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall8(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7, Datum arg8)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 8, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;
	fcinfo.argnull[7] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}

Datum
OidFunctionCall9(Oid functionId, Datum arg1, Datum arg2,
				 Datum arg3, Datum arg4, Datum arg5,
				 Datum arg6, Datum arg7, Datum arg8,
				 Datum arg9)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	fmgr_info(functionId, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 9, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.arg[2] = arg3;
	fcinfo.arg[3] = arg4;
	fcinfo.arg[4] = arg5;
	fcinfo.arg[5] = arg6;
	fcinfo.arg[6] = arg7;
	fcinfo.arg[7] = arg8;
	fcinfo.arg[8] = arg9;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;
	fcinfo.argnull[3] = false;
	fcinfo.argnull[4] = false;
	fcinfo.argnull[5] = false;
	fcinfo.argnull[6] = false;
	fcinfo.argnull[7] = false;
	fcinfo.argnull[8] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", flinfo.fn_oid);

	return result;
}


/*
 * Special cases for convenient invocation of datatype I/O functions.
 */

/*
 * Call a previously-looked-up datatype input function.
 *
 * "str" may be NULL to indicate we are reading a NULL.  In this case
 * the caller should assume the result is NULL, but we'll call the input
 * function anyway if it's not strict.  So this is almost but not quite
 * the same as FunctionCall3.
 */
Datum
InputFunctionCall(FmgrInfo *flinfo, char *str, Oid typioparam, int32 typmod)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	if (str == NULL && flinfo->fn_strict)
		return (Datum) 0;		/* just return null result */

	InitFunctionCallInfoData(fcinfo, flinfo, 3, NULL, NULL);

	fcinfo.arg[0] = CStringGetDatum(str);
	fcinfo.arg[1] = ObjectIdGetDatum(typioparam);
	fcinfo.arg[2] = Int32GetDatum(typmod);
	fcinfo.argnull[0] = (str == NULL);
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Should get null result if and only if str is NULL */
	if (str == NULL)
	{
		if (!fcinfo.isnull)
			elog(ERROR, "input function %u returned non-NULL",
				 fcinfo.flinfo->fn_oid);
	}
	else
	{
		if (fcinfo.isnull)
			elog(ERROR, "input function %u returned NULL",
				 fcinfo.flinfo->fn_oid);
	}

	return result;
}

/*
 * Call a previously-looked-up datatype output function.
 *
 * Do not call this on NULL datums.
 *
 * This is mere window dressing for FunctionCall1, but its use is recommended
 * anyway so that code invoking output functions can be identified easily.
 */
char *
OutputFunctionCall(FmgrInfo *flinfo, Datum val)
{
	return DatumGetCString(FunctionCall1(flinfo, val));
}

/*
 * Call a previously-looked-up datatype binary-input function.
 *
 * "buf" may be NULL to indicate we are reading a NULL.  In this case
 * the caller should assume the result is NULL, but we'll call the receive
 * function anyway if it's not strict.  So this is almost but not quite
 * the same as FunctionCall3.
 */
Datum
ReceiveFunctionCall(FmgrInfo *flinfo, StringInfo buf,
					Oid typioparam, int32 typmod)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	if (buf == NULL && flinfo->fn_strict)
		return (Datum) 0;		/* just return null result */

	InitFunctionCallInfoData(fcinfo, flinfo, 3, NULL, NULL);

	fcinfo.arg[0] = PointerGetDatum(buf);
	fcinfo.arg[1] = ObjectIdGetDatum(typioparam);
	fcinfo.arg[2] = Int32GetDatum(typmod);
	fcinfo.argnull[0] = (buf == NULL);
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Should get null result if and only if buf is NULL */
	if (buf == NULL)
	{
		if (!fcinfo.isnull)
			elog(ERROR, "receive function %u returned non-NULL",
				 fcinfo.flinfo->fn_oid);
	}
	else
	{
		if (fcinfo.isnull)
			elog(ERROR, "receive function %u returned NULL",
				 fcinfo.flinfo->fn_oid);
	}

	return result;
}

/*
 * Call a previously-looked-up datatype binary-output function.
 *
 * Do not call this on NULL datums.
 *
 * This is little more than window dressing for FunctionCall1, but its use is
 * recommended anyway so that code invoking output functions can be identified
 * easily.	Note however that it does guarantee a non-toasted result.
 */
bytea *
SendFunctionCall(FmgrInfo *flinfo, Datum val)
{
	return DatumGetByteaP(FunctionCall1(flinfo, val));
}

/*
 * As above, for I/O functions identified by OID.  These are only to be used
 * in seldom-executed code paths.  They are not only slow but leak memory.
 */
Datum
OidInputFunctionCall(Oid functionId, char *str, Oid typioparam, int32 typmod)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return InputFunctionCall(&flinfo, str, typioparam, typmod);
}

char *
OidOutputFunctionCall(Oid functionId, Datum val)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return OutputFunctionCall(&flinfo, val);
}

Datum
OidReceiveFunctionCall(Oid functionId, StringInfo buf,
					   Oid typioparam, int32 typmod)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return ReceiveFunctionCall(&flinfo, buf, typioparam, typmod);
}

bytea *
OidSendFunctionCall(Oid functionId, Datum val)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return SendFunctionCall(&flinfo, val);
}


/*-------------------------------------------------------------------------
 *		Support routines for toastable datatypes
 *-------------------------------------------------------------------------
 */

struct varlena *
pg_detoast_datum(struct varlena * datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return (struct varlena *) heap_tuple_untoast_attr((varattrib *) datum);
	else
		return datum;
}

struct varlena *
pg_detoast_datum_copy(struct varlena * datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return (struct varlena *) heap_tuple_untoast_attr((varattrib *) datum);
	else
	{
		/* Make a modifiable copy of the varlena object */
		Size		len = VARSIZE(datum);
		struct varlena *result = (struct varlena *) palloc(len);

		memcpy(result, datum, len);
		return result;
	}
}

struct varlena *
pg_detoast_datum_slice(struct varlena * datum, int32 first, int32 count)
{
	/* Only get the specified portion from the toast rel */
	return (struct varlena *) heap_tuple_untoast_attr_slice((varattrib *) datum, first, count);
}

/*-------------------------------------------------------------------------
 *		Support routines for extracting info from fn_expr parse tree
 *
 * These are needed by polymorphic functions, which accept multiple possible
 * input types and need help from the parser to know what they've got.
 *-------------------------------------------------------------------------
 */

/*
 * Get the actual type OID of the function return type
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_rettype(FmgrInfo *flinfo)
{
	Node	   *expr;

	/*
	 * can't return anything useful if we have no FmgrInfo or if its fn_expr
	 * node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	expr = flinfo->fn_expr;

	return exprType(expr);
}

/*
 * Get the actual type OID of a specific function argument (counting from 0)
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_argtype(FmgrInfo *flinfo, int argnum)
{
	/*
	 * can't return anything useful if we have no FmgrInfo or if its fn_expr
	 * node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	return get_call_expr_argtype(flinfo->fn_expr, argnum);
}

/*
 * Get the actual type OID of a specific function argument (counting from 0),
 * but working from the calling expression tree instead of FmgrInfo
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_call_expr_argtype(Node *expr, int argnum)
{
	List	   *args;
	Oid			argtype;

	if (expr == NULL)
		return InvalidOid;

	if (IsA(expr, FuncExpr))
		args = ((FuncExpr *) expr)->args;
	else if (IsA(expr, OpExpr))
		args = ((OpExpr *) expr)->args;
	else if (IsA(expr, DistinctExpr))
		args = ((DistinctExpr *) expr)->args;
	else if (IsA(expr, ScalarArrayOpExpr))
		args = ((ScalarArrayOpExpr *) expr)->args;
	else if (IsA(expr, NullIfExpr))
		args = ((NullIfExpr *) expr)->args;
	else
		return InvalidOid;

	if (argnum < 0 || argnum >= list_length(args))
		return InvalidOid;

	argtype = exprType((Node *) list_nth(args, argnum));

	/*
	 * special hack for ScalarArrayOpExpr: what the underlying function will
	 * actually get passed is the element type of the array.
	 */
	if (IsA(expr, ScalarArrayOpExpr) &&
		argnum == 1)
		argtype = get_element_type(argtype);

	return argtype;
}
