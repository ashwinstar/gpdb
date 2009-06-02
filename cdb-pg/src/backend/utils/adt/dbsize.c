/*
 * dbsize.c
 *		object size functions
 *
 * Copyright (c) 2002-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/dbsize.c,v 1.9.2.1 2007/03/11 06:44:11 alvherre Exp $
 *
 */

#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <glob.h>

#include "access/heapam.h"
#include "access/appendonlywriter.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/relcache.h"

#include "gp-libpq-fe.h"
#include "lib/stringinfo.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/inval.h"


static int64
get_size_from_segDBs(const char * cmd)
{

	int64	size = 0;
	int		i;
	int 	resultCount = 0;
	struct pg_result **results = NULL;
	
	StringInfoData errbuf;

	initStringInfo(&errbuf);

	results = cdbdisp_dispatchRMCommand(cmd, true, false, &errbuf, &resultCount);

	if (errbuf.len > 0)
		ereport(ERROR, (errmsg("dbsize error (gathered %d results from cmd '%s')", resultCount, cmd),
						errdetail("%s", errbuf.data)));
										
	for (i = 0; i < resultCount; i++)
	{
		if (PQresultStatus(results[i]) != PGRES_TUPLES_OK)
		{
			elog(ERROR,"dbsize: resultStatus not tuples_Ok");
		}
		else
		{
			/*
			 * Due to funkyness in the current dispatch agent code, instead of 1 result 
			 * per QE with 1 row each, we can get back 1 result per dispatch agent, with
			 * one row per QE controlled by that agent.
			 */
			int j;
			for (j = 0; j < PQntuples(results[i]); j++)
			{
				int64 tempsize = 0;
				(void) scanint8(PQgetvalue(results[i], j, 0), false, &tempsize);
				 size += tempsize;
			}
		}
	}

	pfree(errbuf.data);

	for (i = 0; i < resultCount; i++)
		PQclear(results[i]);

	free(results);
	
	return size;
}

/* Return physical size of directory contents, or 0 if dir doesn't exist */
static int64
db_dir_size(const char *path)
{
	int64		dirsize = 0;
	struct dirent *direntry;
	DIR		   *dirdesc;
	char		filename[MAXPGPATH];

	dirdesc = AllocateDir(path);

	if (!dirdesc)
		return 0;

	while ((direntry = ReadDir(dirdesc, path)) != NULL)
	{
		struct stat fst;

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(filename, MAXPGPATH, "%s/%s", path, direntry->d_name);

		if (stat(filename, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", filename)));
		}
		dirsize += fst.st_size;
	}

	FreeDir(dirdesc);
	return dirsize;
}

/*
 * calculate size of database in all tablespaces
 */
static int64
calculate_database_size(Oid dbOid)
{
	int64		totalsize;
	DIR		   *dirdesc;
	struct dirent *direntry;
	char		dirpath[MAXPGPATH];
	char		pathname[MAXPGPATH];

	/* Shared storage in pg_global is not counted */

	/* Include pg_default storage */
	snprintf(pathname, MAXPGPATH, "base/%u", dbOid);
	totalsize = db_dir_size(pathname);

	/* Scan the non-default tablespaces */
	snprintf(dirpath, MAXPGPATH, "pg_tblspc");
	dirdesc = AllocateDir(dirpath);
	if (!dirdesc)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tablespace directory \"%s\": %m",
						dirpath)));

	while ((direntry = ReadDir(dirdesc, dirpath)) != NULL)
	{
		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(pathname, MAXPGPATH, "pg_tblspc/%s/%u",
				 direntry->d_name, dbOid);
		totalsize += db_dir_size(pathname);
	}

	FreeDir(dirdesc);

	/* Complain if we found no trace of the DB at all */
	if (!totalsize)
		ereport(ERROR,
				(ERRCODE_UNDEFINED_DATABASE,
				 errmsg("database with OID %u does not exist", dbOid)));

	return totalsize;
}

Datum
pg_database_size_oid(PG_FUNCTION_ARGS)
{
	int64		size = 0;
	Oid			dbOid = PG_GETARG_OID(0);
	size = calculate_database_size(dbOid);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		StringInfoData buffer;
		
		initStringInfo(&buffer);

		appendStringInfo(&buffer, "select pg_database_size(%u)", dbOid);
		
		size += get_size_from_segDBs(buffer.data);
	}

	PG_RETURN_INT64(size);
}

Datum
pg_database_size_name(PG_FUNCTION_ARGS)
{
	int64		size = 0;
	Name		dbName = PG_GETARG_NAME(0);
	Oid			dbOid = get_database_oid(NameStr(*dbName));

	if (!OidIsValid(dbOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist",
						NameStr(*dbName))));
						
	size = calculate_database_size(dbOid);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		StringInfoData buffer;
		
		initStringInfo(&buffer);

		appendStringInfo(&buffer, "select pg_database_size('%s')", quote_identifier(NameStr(*dbName)));
		
		size += get_size_from_segDBs(buffer.data);
	}

	PG_RETURN_INT64(size);
}


/*
 * calculate total size of tablespace
 */
static int64
calculate_tablespace_size(Oid tblspcOid)
{
	char		tblspcPath[MAXPGPATH];
	char		pathname[MAXPGPATH];
	int64		totalsize = 0;
	DIR		   *dirdesc;
	struct dirent *direntry;

	if (tblspcOid == DEFAULTTABLESPACE_OID)
		snprintf(tblspcPath, MAXPGPATH, "base");
	else if (tblspcOid == GLOBALTABLESPACE_OID)
		snprintf(tblspcPath, MAXPGPATH, "global");
	else
		snprintf(tblspcPath, MAXPGPATH, "pg_tblspc/%u", tblspcOid);

	dirdesc = AllocateDir(tblspcPath);

	if (!dirdesc)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tablespace directory \"%s\": %m",
						tblspcPath)));

	while ((direntry = ReadDir(dirdesc, tblspcPath)) != NULL)
	{
		struct stat fst;

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(pathname, MAXPGPATH, "%s/%s", tblspcPath, direntry->d_name);

		if (stat(pathname, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathname)));
		}

		if (S_ISDIR(fst.st_mode))
			totalsize += db_dir_size(pathname);

		totalsize += fst.st_size;
	}

	FreeDir(dirdesc);

	return totalsize;
}

Datum
pg_tablespace_size_oid(PG_FUNCTION_ARGS)
{
	int64		size = 0;
	Oid			tblspcOid = PG_GETARG_OID(0);

	size = calculate_tablespace_size(tblspcOid);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		StringInfoData buffer;
		
		initStringInfo(&buffer);
		
		appendStringInfo(&buffer, "select pg_tablespace_size(%u)", tblspcOid);
		
		size += get_size_from_segDBs(buffer.data);
	}

	PG_RETURN_INT64(size);
}

Datum
pg_tablespace_size_name(PG_FUNCTION_ARGS)
{
	int64		size = 0;
	Name		tblspcName = PG_GETARG_NAME(0);
	Oid			tblspcOid = get_tablespace_oid(NameStr(*tblspcName));

	if (!OidIsValid(tblspcOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						NameStr(*tblspcName))));

	size = calculate_tablespace_size(tblspcOid);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		StringInfoData buffer;
		
		initStringInfo(&buffer);
		
		appendStringInfo(&buffer, "select pg_tablespace_size('%s')", quote_identifier(NameStr(*tblspcName)));
		
		size += get_size_from_segDBs(buffer.data);
	}

	PG_RETURN_INT64(size);
}

/*
 * calculate size of a relation
 *
 * Iterator over all files belong to the relation and do stat.
 * The obviously better way is to use glob.  For whatever reason,
 * glob is extremely slow if there are lots of relations in the
 * database.  So we handle all cases, instead. 
 */
static int64
calculate_relation_size(Relation rel)
{
	int64		totalsize = 0;
	char	   *relationpath;
	char		pathname[MAXPGPATH];

    char relstorage; 
    struct stat fst;
    int i;

	relationpath = relpath(rel->rd_node);
    relstorage = rel->rd_rel->relstorage;

    if(relstorage == RELSTORAGE_HEAP)
    {
        /* Ordinary relation, including heap and index.
         * They take form of relationpath, or relationpath.%d
         * There will be no holes, therefore, we can stop we
         * we reach the first non-exist file.
         */
        for(i=0; ; ++i)
        {
            if (i==0)
                snprintf(pathname, MAXPGPATH, "%s", relationpath); 
            else
                snprintf(pathname, MAXPGPATH, "%s.%d", relationpath, i);

            if (stat(pathname, &fst) >= 0)
                totalsize += fst.st_size;
            else
            {
                if (errno == ENOENT)
                    break;
                else
                    ereport(ERROR, (errcode_for_file_access(), 
                                    errmsg("could not stat file %s: %m", pathname)
                                ));
            }
        }
    }
    else if (relstorage == RELSTORAGE_AO)
    {
        /* AO: same as heap, it will look like relationpath, or relationpath.%d,
         * but it may contains holes.  We need to looping till reach MAX_AOREL_CONCURRENCY.
         */
        for(i=0; i<MAX_AOREL_CONCURRENCY; ++i)
        {
            if (i==0)
                snprintf(pathname, MAXPGPATH, "%s", relationpath); 
            else
                snprintf(pathname, MAXPGPATH, "%s.%d", relationpath, i);

            if (stat(pathname, &fst) >= 0)
                totalsize += fst.st_size;
            else 
            {
                if (errno != ENOENT)
                    ereport(ERROR, (errcode_for_file_access(), 
                                    errmsg("could not stat file %s: %m", pathname)
                                ));
            }
        }
    }
    else if (relstorage == RELSTORAGE_AOCS)
    {
        /* AOCS: It looks like relpath.col.segno.  Also, segno may have
         * holes, so we will test till MAX_AOREL_CONCURRENCY. 
         */
        int nvp = rel->rd_att->natts;
        int j = 0;

        for(i=0; i<MAX_AOREL_CONCURRENCY; ++i)
        {
            for(j=0; j<nvp; ++j)
            {
                snprintf(pathname, MAXPGPATH, "%s.%d.%d", relationpath, j, i);
 
                if (stat(pathname, &fst) >= 0)
                    totalsize += fst.st_size;
                else 
                {
                    if (errno == ENOENT)
                        break;
                    else
                        ereport(ERROR, (errcode_for_file_access(), 
                                    errmsg("could not stat file %s: %m", pathname)
                                    ));
                }
            }
        }
    }
           
    /* RELSTORAGE_VIRTUAL has no space usage */
    return totalsize;
}

Datum
pg_relation_size_oid(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size = 0;

	rel = relation_open(relOid, AccessShareLock);
	
	if (relOid == 0 || rel->rd_node.relNode == 0)
		size = 0;
	else
		size = calculate_relation_size(rel); 
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		StringInfoData buffer;
		char *schemaName;
		char *relName;

		schemaName = get_namespace_name(get_rel_namespace(relOid));
		if (schemaName == NULL)
		{
			elog(ERROR, "Cannot find schema for oid %d", relOid);
		}

		relName = get_rel_name(relOid);
		if (relName == NULL)
		{
			elog(ERROR, "Cannot find relation for oid %d", relOid);
		}

		initStringInfo(&buffer);
		
		appendStringInfo(&buffer, "select pg_relation_size('%s.%s')", quote_identifier(schemaName), quote_identifier(relName));

		size += get_size_from_segDBs(buffer.data);
	}

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

Datum
pg_relation_size_name(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	RangeVar   *relrv;
	Relation	rel;
	int64		size;
	
	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	
	if (Gp_role == GP_ROLE_EXECUTE && relrv->schemaname != NULL)
	{
		Oid namespaceId;
		Oid relOid;
		/*
		 * Do this the hard way, because the optimizer wants to be able to use this
		 * function on relations the user might not have direct access to.
		 */

		AcceptInvalidationMessages();
		
		namespaceId =  GetSysCacheOid(NAMESPACENAME,
									  CStringGetDatum(relrv->schemaname),
									  0, 0, 0);
		relOid = get_relname_relid(relrv->relname, namespaceId);
		
		if (relOid == InvalidOid)
		{
			size = 0;
			PG_RETURN_INT64(size);
		}
		
		/* Let relation_open do the rest */
		rel = relation_open(relOid, AccessShareLock);
	}
	else
		rel = relation_openrv(relrv, AccessShareLock);
	
	if (rel->rd_node.relNode == 0)
		size = 0;
	else
		size = calculate_relation_size(rel); 
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		char * rawname;
		StringInfoData buffer;
		
		initStringInfo(&buffer);
		
		rawname = DatumGetCString(DirectFunctionCall1(textout,
													  PointerGetDatum(relname)));
		
		appendStringInfo(&buffer, "select pg_relation_size('%s')", rawname);
		
		size += get_size_from_segDBs(buffer.data);
	}

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}


/*
 *	Compute the on-disk size of files for the relation according to the
 *	stat function, including heap data, index data, toast data, and aoseg data.
 */
static int64
calculate_total_relation_size(Oid Relid)
{
	Relation	heapRel;
	Oid			toastOid;
	Oid			aosegOid;
	int64		size;
	ListCell   *cell;

	heapRel = relation_open(Relid, AccessShareLock);
	toastOid = heapRel->rd_rel->reltoastrelid;
	aosegOid = heapRel->rd_rel->relaosegrelid;
	
	/* Get the heap size */
	if (Relid == 0 || heapRel->rd_node.relNode == 0)
		size = 0;
	else
		size = calculate_relation_size(heapRel); 

	/* Include any dependent indexes */
	if (heapRel->rd_rel->relhasindex)
	{
		List	   *index_oids = RelationGetIndexList(heapRel);

		foreach(cell, index_oids)
		{
			Oid			idxOid = lfirst_oid(cell);
			Relation	iRel;

			iRel = relation_open(idxOid, AccessShareLock);

			size += calculate_relation_size(iRel); 

			relation_close(iRel, AccessShareLock);
		}

		list_free(index_oids);
	}

	/* Recursively include toast table (and index) size */
	if (OidIsValid(toastOid))
		size += calculate_total_relation_size(toastOid);

	if (OidIsValid(aosegOid))
		size += calculate_total_relation_size(aosegOid);

	relation_close(heapRel, AccessShareLock);

	return size;
}

Datum
pg_total_relation_size_oid(PG_FUNCTION_ARGS)
{
	int64		size = 0;
	Oid			relOid = PG_GETARG_OID(0);
	
	size = calculate_total_relation_size(relOid);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		StringInfoData buffer;
		char *schemaName;
		char *relName;

		schemaName = get_namespace_name(get_rel_namespace(relOid));
		if (schemaName == NULL)
		{
			elog(ERROR, "Cannot find schema for oid %d", relOid);
		}

		relName = get_rel_name(relOid);
		if (relName == NULL)
		{
			elog(ERROR, "Cannot find relation for oid %d", relOid);
		}

		initStringInfo(&buffer);
		appendStringInfo(&buffer, "select pg_total_relation_size('%s.%s')", quote_identifier(schemaName), quote_identifier(relName));

		size += get_size_from_segDBs(buffer.data);
	}

	PG_RETURN_INT64(size);
}

Datum
pg_total_relation_size_name(PG_FUNCTION_ARGS)
{
	int64		size = 0;
	text	   *relname = PG_GETARG_TEXT_P(0);
	RangeVar   *relrv;
	Oid			relid;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	
	if (Gp_role == GP_ROLE_EXECUTE && relrv->schemaname != NULL)
	{
		Oid namespaceId;
		/*
		 * Do this the hard way to avoid access check we don't want
		 */
		AcceptInvalidationMessages();

		namespaceId =  GetSysCacheOid(NAMESPACENAME,
								 CStringGetDatum(relrv->schemaname),
								 0, 0, 0);
		relid = get_relname_relid(relrv->relname, namespaceId);
	}
	else
		relid = RangeVarGetRelid(relrv, false);

	size = calculate_total_relation_size(relid);
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		char * rawname;
		StringInfoData buffer;
		
		initStringInfo(&buffer);
		
		rawname = DatumGetCString(DirectFunctionCall1(textout,
													  PointerGetDatum(relname)));
		
		appendStringInfo(&buffer, "select pg_total_relation_size('%s')", rawname);
		
		size += get_size_from_segDBs(buffer.data);
	}

	PG_RETURN_INT64(size);
}

/*
 * formatting with size units
 */
Datum
pg_size_pretty(PG_FUNCTION_ARGS)
{
	int64		size = PG_GETARG_INT64(0);
	char	   *result = palloc(50 + VARHDRSZ);
	int64		limit = 10 * 1024;
	int64		mult = 1;

	if (size < limit * mult)
		snprintf(VARDATA(result), 50, INT64_FORMAT " bytes", size);
	else
	{
		mult *= 1024;
		if (size < limit * mult)
			snprintf(VARDATA(result), 50, INT64_FORMAT " kB",
					 (size + mult / 2) / mult);
		else
		{
			mult *= 1024;
			if (size < limit * mult)
				snprintf(VARDATA(result), 50, INT64_FORMAT " MB",
						 (size + mult / 2) / mult);
			else
			{
				mult *= 1024;
				if (size < limit * mult)
					snprintf(VARDATA(result), 50, INT64_FORMAT " GB",
							 (size + mult / 2) / mult);
				else
				{
					mult *= 1024;
					snprintf(VARDATA(result), 50, INT64_FORMAT " TB",
							 (size + mult / 2) / mult);
				}
			}
		}
	}

	SET_VARSIZE(result, strlen(VARDATA(result)) + VARHDRSZ);

	PG_RETURN_TEXT_P(result);
}
