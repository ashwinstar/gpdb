/*-------------------------------------------------------------------------
 *
 * filespace.c
 *	  Commands to manipulate filespaces
 *
 * Copyright (c) 2009-2010 Greenplum Inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"

/* System libraries for file and directory operations */
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_filespace_entry.h"
#include "commands/filespace.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "cdb/cdbdisp.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"

static void checkpath(char*);
static void checkpath2(char*);

static void DeleteFilespaceEntryTuples(Oid fsoid);

/*
 * Create a filespace
 *
 * Only superusers can create a filespace. This seems a reasonable restriction
 * since we're determining the system layout and, anyway, we probably have
 * root if we're doing this kind of activity
 */
void
CreateFileSpace(CreateFileSpaceStmt *stmt)
{
	Relation     rel;	
	HeapTuple	 tuple;
	TupleDesc    tupdesc;
	NameData     fsname;       /* filespace name */
	Oid          ownerId;      /* OID of the OWNER of the filespace */
	Oid          fsoid;        /* OID of the created filespace */
	ListCell    *cell;         /* List loop variable */
	int          i;            /* Array loop variable */
	bool         nulls[Natts_pg_filespace];
	Datum        values[Natts_pg_filespace];
	bool         enulls[Natts_pg_filespace_entry];
	Datum        evalues[Natts_pg_filespace_entry];

	/* validate */

	/* don't call this in a transaction block */
	/* May need to make this "subtransaction block" */
	// PreventTransactionChain((void *) stmt, "CREATE FILESPACE");

	/* Must be super user */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create filespace \"%s\"",
						stmt->filespacename),
				 errhint("Must be superuser to create a filespace.")));

	/* However, the eventual owner of the filespace need not be */
	if (stmt->owner)
		ownerId = get_roleid_checked(stmt->owner);
	else
		ownerId = GetUserId();

	/*
	 * Disallow creation of filespaces named "pg_xxx"; we reserve this
	 * namespace for system purposes.
	 */
	if (!allowSystemTableMods && IsReservedName(stmt->filespacename))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable filespace name \"%s\"",
						stmt->filespacename),
		errdetail("The prefix \"pg_\" is reserved for system filespaces.")));
	namestrcpy(&fsname, stmt->filespacename);

	/*
	 * Because rollback of filespace creation is unpleasant we prefer to
	 * ensure that we fully serialize CREATE FILESPACE operations.  Therfore
	 * we take a big lock up-front.
	 */
	rel = heap_open(FileSpaceRelationId, AccessExclusiveLock);

	/* Check that there is no other filespace by this name. */
	fsoid = get_filespace_oid(rel, stmt->filespacename);
	if (OidIsValid(fsoid))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("filespace \"%s\" already exists",
						stmt->filespacename)));

	/* Check specification against configuration information */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CdbComponentDatabases *segments;
		int                    numsegs;

		/* 
		 * We need to have a path specified for every segment
		 * This is an N^2 algorithm on number of segments.
		 * we can probably do better
		 */
		segments = getCdbComponentDatabases();

		/* First check that the master and standby master are accounted for */
		for (i = 0; i < segments->total_entry_dbs; i++)
		{
			int  segid = segments->entry_db_info[i].dbid;
			bool found = false;

			foreach(cell, stmt->locations)
			{
				DefElem *defel = (DefElem *) lfirst(cell);
				int      dbid  = intVal(defel->arg);

				if (dbid == segid)
				{
					if (found)
						ereport(ERROR,
								(errcode(ERRCODE_GP_COMMAND_ERROR),
								 errmsg("duplicate segment id %d", segid)));
					found = true;
				}
			}
			
			if (!found)
			{
				ereport(ERROR,
						(errcode(ERRCODE_GP_COMMAND_ERROR),
						 errmsg("no path specified for segment id %d", segid)));
			}
		}

		/* Then check that all segments are accounted for */
		for (i = 0; i < segments->total_segment_dbs; i++)
		{
			int  segid = segments->segment_db_info[i].dbid;
			bool found = false;

			foreach(cell, stmt->locations)
			{
				DefElem *defel = (DefElem *) lfirst(cell);
				int      dbid  = intVal(defel->arg);

				if (dbid == segid)
				{
					if (found)
						ereport(ERROR,
								(errcode(ERRCODE_GP_COMMAND_ERROR),
								 errmsg("duplicate segment id %d", segid)));
					found = true;
				}
			}
			if (!found)
			{
				ereport(ERROR,
						(errcode(ERRCODE_GP_COMMAND_ERROR),
						 errmsg("no path specified for segment id %d", segid)));
			}
		}

		/* 
		 * Finally check that we haven't specified anything "extra".
		 * First check that the count matches since this is the expected
		 * result, only perform the N^2 check for the invalid entry when
		 * we know that something is wrong.
		 */
		numsegs = 0;
		foreach(cell, stmt->locations)
		{
			DefElem *defel = (DefElem *) lfirst(cell);
			char    *path  = defel->defname;

			checkpath(path);  /* Simple validation of paths */
			numsegs++;
		}

		if (numsegs != segments->total_segments)
		{
			foreach(cell, stmt->locations)
			{
				DefElem *defel = (DefElem *) lfirst(cell);
				int      dbid  = intVal(defel->arg);
				bool     found = false;

				for (i = 0; !found && i < segments->total_entry_dbs; i++)
				{
					int  segid = segments->entry_db_info[i].dbid;
					if (segid == dbid)
						found = true;
				}
				for (i = 0; !found && i < segments->total_segment_dbs; i++)
				{
					int  segid = segments->segment_db_info[i].dbid;
					if (segid == dbid)
						found = true;
				}
				
				if (!found)
				{
					ereport(ERROR,
							(errcode(ERRCODE_GP_COMMAND_ERROR),
							 errmsg("Specified Segment id %d does not exist "
									"in configuration", dbid)));
				}
			}
		}
	}

	/* 
	 * Find the location of the filespace for *this* segment, and perform 
	 * additional validations on the path.
	 */
	foreach(cell, stmt->locations)
	{
		DefElem *defel = (DefElem *) lfirst(cell);
		int      dbid  = intVal(defel->arg);	
		char    *path  = defel->defname;

		if (dbid == GpIdentity.dbid)
			checkpath2(path);  /* Additional path checks */
	}

	/* We opened the relation up at the top of the function */
	Assert(rel);

	/* Insert tuple into pg_filespace */
	tupdesc = RelationGetDescr(rel);
	MemSet(nulls, false, sizeof(nulls));
	values[Anum_pg_filespace_fsname - 1]  = NameGetDatum(&fsname);
	values[Anum_pg_filespace_fsowner - 1] = ObjectIdGetDatum(ownerId);
	tuple = heap_form_tuple(tupdesc, values, nulls);

	/* Keep oids synchonized between master and segments */
	if (OidIsValid(stmt->fsoid))
		HeapTupleSetOid(tuple, stmt->fsoid);
	fsoid = simple_heap_insert(rel, tuple);
	Assert(OidIsValid(fsoid));

	CatalogUpdateIndexes(rel, tuple);
	heap_freetuple(tuple);

	/* Record dependency on owner */
	recordDependencyOnOwner(FileSpaceRelationId, fsoid, ownerId);

	/* Keep the lock until commit/abort */
	heap_close(rel, NoLock);

	/* 
	 * Master only:
	 *   1) Add all locations to the pg_filespace_entry table
	 *   2) Dispatch to the segments
	 *
	 * Note: we keep the filespace_entry catalog a master-only catalog
	 * because this makes expansion and adding mirrors an easier process:
	 *   - We can add them to the catalog to the master in utility mode
	 *     while the system is down.
	 *   - There isn't a bootstrapping problem of which to create first
	 *     the new segment or the catalog that describes it.
	 */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		rel = heap_open(FileSpaceEntryRelationId, RowExclusiveLock);
		tupdesc = RelationGetDescr(rel);
		MemSet(enulls, false, sizeof(enulls));
		evalues[Anum_pg_filespace_entry_fsefsoid - 1] = ObjectIdGetDatum(fsoid);

		foreach(cell, stmt->locations)
		{
			DefElem *defel = (DefElem *) lfirst(cell);
			int4     dbid  = intVal(defel->arg);	
			char    *path  = defel->defname;
			
			evalues[Anum_pg_filespace_entry_fsedbid - 1] = Int32GetDatum(dbid);
			evalues[Anum_pg_filespace_entry_fselocation - 1 ] =
				DirectFunctionCall1(textin, CStringGetDatum(path));
			
			tuple = heap_form_tuple(tupdesc, evalues, enulls);
			simple_heap_insert(rel, tuple);
			CatalogUpdateIndexes(rel, tuple);
			heap_freetuple(tuple);
		}

		heap_close(rel, RowExclusiveLock);

		/* Dispatch to segments */
		stmt->fsoid = fsoid;  /* Already Asserted OidIsValid */
		CdbDispatchUtilityStatement((Node *) stmt, "CreateFilespaceCommand");

		/* MPP-6929: metadata tracking */
		MetaTrackAddObject(FileSpaceRelationId,
						   fsoid,
						   GetUserId(),
						   "CREATE", "FILESPACE"
				);
	}
}

/*
 * Drop a filespace
 *
 * Be careful to check that the filespace is empty.
 */
void 
RemoveFileSpace(List *names, DropBehavior behavior, bool missing_ok)
{
	Relation      rel;
	char         *fsname;
	Oid			  fsoid;
	ObjectAddress object;

	/* don't call this in a transaction block */
	/* May need to make this "subtransaction block" */
	// PreventTransactionChain((void *) stmt, "DROP FILESPACE");

	/* 
	 * General DROP (object) syntax allows fully qualified names, but
	 * filespaces are global objects that do not live in schemas, so
	 * it is a syntax error if a fully qualified name was given.
	 */
	if (list_length(names) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("filespace name may not be qualified")));
	fsname = strVal(linitial(names));

	/* Disallow CASCADE */
	if (behavior == DROP_CASCADE)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax at or near \"cascade\"")));

	/* 
	 * Because rollback of filespace operations are difficult and expected
	 * usage is anticipated to be light we remove concurency worries by
	 * taking a big lock up front.
	 */
	rel = heap_open(FileSpaceRelationId, AccessExclusiveLock);

	/* Lookup the name in pg_filespace */
	fsoid = get_filespace_oid(rel, fsname);
	if (!OidIsValid(fsoid))
	{
		heap_close(rel, AccessExclusiveLock);

		if (missing_ok)
		{
			if (Gp_role != GP_ROLE_EXECUTE)
				ereport(NOTICE,
						(errmsg("filespace \"%s\" does not exist, skipping", 
								fsname)));
			return;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("filespace \"%s\" does not exist", fsname)));
		}
	}

	/* Must be owner */
	if (!pg_filespace_ownercheck(fsoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_FILESPACE, fsname);

	/* Disallow drop of the standard filespaces, even by superuser */
	if (fsoid == SYSTEMFILESPACE_OID)
		ereport(ERROR,
				(errmsg("cannot drop filespace %s because it is required "
						"by the database system", fsname)));

	/* Check for dependencies and remove the filespace */
	object.classId = FileSpaceRelationId;
	object.objectId = fsoid;
	object.objectSubId = 0;
	performDeletion(&object, DROP_RESTRICT);

	/* Keep the lock until commit/abort */
	heap_close(rel, NoLock);

	/* 
	 * Master Only:
	 *   1) Remove entries from pg_filespace_entry
	 *
	 * Note: no need for dispatch, that is handled in utility.c
	 */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		DeleteFilespaceEntryTuples(fsoid);

		/* MPP-6929: metadata tracking */
		MetaTrackDropObject(FileSpaceRelationId,
							fsoid);
	}
}

/* 
 * RemoveFileSpaceById
 *   Guts of Filespace Deletion, called by dependency.c
 */
void
RemoveFileSpaceById(Oid fsoid)
{
	Relation	 rel;
	HeapScanDesc scandesc;
	HeapTuple	 tuple;
	ScanKeyData  entry[1];

	/* Lookup the tuple in pg_filespace */
	rel = heap_open(FileSpaceRelationId, RowExclusiveLock);
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fsoid));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (!HeapTupleIsValid(tuple)) /* shouldn't happen */
		elog(ERROR, "cache lookup failed for filespace %u", fsoid);

	simple_heap_delete(rel, &tuple->t_self);

	heap_endscan(scandesc);
	heap_close(rel, RowExclusiveLock);
}

static void 
DeleteFilespaceEntryTuples(Oid fsoid)
{
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/* Grab an appropriate lock on the pg_filespace_entry relation */
	rel = heap_open(FileSpaceEntryRelationId, RowExclusiveLock);

	/* Use the index to scan only attributes of the target relation */
	ScanKeyInit(&entry[0],
				Anum_pg_filespace_entry_fsefsoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fsoid));

	scan = systable_beginscan(rel, FilespaceEntryFsIndexId, true,
							  SnapshotNow, 1, entry);

	/* Delete all the matching tuples */
	while ((tuple = systable_getnext(scan)) != NULL)
		simple_heap_delete(rel, &tuple->t_self);

	/* Clean up after the scan */
	systable_endscan(scan);
	heap_close(rel, RowExclusiveLock);
}


/*
 * Change filespace owner
 */
void
AlterFileSpaceOwner(List *names, Oid newOwnerId)
{
	char             *fsname;
	Oid               fsoid;
	Relation	      rel;
	ScanKeyData       entry[1];
	HeapScanDesc      scandesc;
	Form_pg_filespace fsForm;
	HeapTuple	      tup;

	/*
	 * This was from a generic AltrStmt node which allows for fully qualified
	 * object names, but filespaces don't exist inside schemas so fully
	 * qualified names are a syntax error.
	 */
	if (list_length(names) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("filespace name may not be qualified")));
	fsname = strVal(linitial(names));

	/* Search pg_filespace */
	rel = heap_open(FileSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_filespace_fsname, BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(fsname));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tup = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("filespace \"%s\" does not exist", fsname)));
	fsoid  = HeapTupleGetOid(tup);
	fsForm = (Form_pg_filespace) GETSTRUCT(tup);

	/* Cannot alter system filespaces */
	if (!allowSystemTableMods && IsReservedName(fsname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("permission denied: \"%s\" is a system filespace", 
						fsname)));

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (fsForm->fsowner != newOwnerId)
	{
		Datum		values[Natts_pg_filespace];
		bool		nulls[Natts_pg_filespace];
		bool		replace[Natts_pg_filespace];
		HeapTuple	newtuple;
		TupleDesc   tupdesc;

		/* Otherwise, must be owner of the existing object */
		if (!pg_filespace_ownercheck(fsoid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_FILESPACE, fsname);

		/* Must be able to become new owner */
		check_is_member_of_role(GetUserId(), newOwnerId);

		/*
		 * Normally we would also check for create permissions here, but there
		 * are none for filespaces so we follow what rename filespace does
		 * and omit the create permissions check.
		 *
		 * NOTE: Only superusers may create filespaces to begin with and so
		 * initially only a superuser would be able to change its ownership
		 * anyway.
		 */
		memset(nulls, false, sizeof(nulls));
		memset(replace, false, sizeof(replace));

		replace[Anum_pg_filespace_fsowner - 1] = true;
		values[Anum_pg_filespace_fsowner - 1] = ObjectIdGetDatum(newOwnerId);

		tupdesc = RelationGetDescr(rel);
		newtuple = heap_modify_tuple(tup, tupdesc, values, nulls, replace);

		simple_heap_update(rel, &newtuple->t_self, newtuple);

		/* MPP-6929: metadata tracking */
		if (Gp_role == GP_ROLE_DISPATCH)
			MetaTrackUpdObject(FileSpaceRelationId,
							   fsoid,
							   GetUserId(),
							   "ALTER", "OWNER"
					);

		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(FileSpaceRelationId, fsoid, newOwnerId);
	}

	heap_endscan(scandesc);
	heap_close(rel, RowExclusiveLock);
}


/*
 * Rename a filespace
 */
void
RenameFileSpace(const char *oldname, const char *newname)
{
	Relation	 rel;
	Oid          fsoid;
	ScanKeyData  entry[1];
	HeapScanDesc scan;
	HeapTuple	 tup;
	HeapTuple	 newtuple;
	Form_pg_filespace newform;

	/* Search pg_filespace */
	rel = heap_open(FileSpaceRelationId, RowExclusiveLock);
	ScanKeyInit(&entry[0],
				Anum_pg_filespace_fsname, BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(oldname));
	scan = heap_beginscan(rel, SnapshotNow, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("filespace \"%s\" does not exist",
						oldname)));
	newtuple = heap_copytuple(tup);
	newform = (Form_pg_filespace) GETSTRUCT(newtuple);
	heap_endscan(scan);

	/* Can't rename system filespaces */
	if (!allowSystemTableMods && IsReservedName(oldname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("permission denied: \"%s\" is a system filespace", 
						oldname)));

	/* Must be owner */
	fsoid = HeapTupleGetOid(newtuple);
	if (!pg_filespace_ownercheck(fsoid, GetUserId()))
		aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_FILESPACE, oldname);

	/* Validate new name */
	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable filespace name \"%s\"", newname),
		errdetail("The prefix \"pg_\" is reserved for system filespaces.")));

	/* Make sure the new name doesn't exist */
	ScanKeyInit(&entry[0],
				Anum_pg_filespace_fsname, BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(newname));
	scan = heap_beginscan(rel, SnapshotNow, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("filespace \"%s\" already exists", newname)));
	heap_endscan(scan);

	/* OK, update the entry */
	namestrcpy(&(newform->fsname), newname);

	simple_heap_update(rel, &newtuple->t_self, newtuple);

	/* MPP-6929: metadata tracking */
	if (Gp_role == GP_ROLE_DISPATCH)
		MetaTrackUpdObject(FileSpaceRelationId,
						   fsoid,
						   GetUserId(),
						   "ALTER", "RENAME"
				);

	CatalogUpdateIndexes(rel, newtuple);

	heap_close(rel, RowExclusiveLock);
}



/*
 * get_filespace_name - given a filespace OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such filespace
 */
char *
get_filespace_name(Oid fsoid)
{
	char       *fsname;
	char	   *result;
	Relation	rel;
	HeapScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_filespace.  We use a heapscan here even though there is an
	 * index on oid, on the theory that pg_filespace will usually have just a
	 * few entries and so an indexed lookup is a waste of effort.
	 */
	rel = heap_open(FileSpaceRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fsoid));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
	{
		fsname = NameStr(((Form_pg_filespace) GETSTRUCT(tuple))->fsname);
		result = pstrdup(fsname);
	}
	else
	{
		result = NULL;
	}

	heap_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	return result;
}


/*
 * get_filespace_oid - given a filespace name, look up the OID
 *
 * Returns InvalidOid if filespace name not found.
 *
 * If a lockmode of anything other than NoLock is specified we retain
 * the lock on return, and will only release it on commit/abort.
 */
Oid
get_filespace_oid(Relation rel, const char *filespacename)
{
	Oid			 result;
	HeapScanDesc scandesc;
	HeapTuple	 tuple;
	ScanKeyData  entry[1];

	ScanKeyInit(&entry[0],
				Anum_pg_filespace_fsname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(filespacename));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = HeapTupleGetOid(tuple);
	else
		result = InvalidOid;

	heap_endscan(scandesc);
	return result;
}


/* 
 * Commands for accessing the filespace oid -> directory lookup cache.
 *
 * These directories are not stored in the catalog except on the master,
 * for segments they are passed to the segment on startup, so we must 
 * track their values through this alternative mechanism.
 */
void SetFilespaceDirectory(Oid fsoid, const char *directory)
{
	/* Can be called during bootstrap so it shouldn't raise errors */
	/* NOOP */
}

void ClearFilespaceDirectory(Oid fsoid)
{
	/* NOOP */
}

char* GetFilespaceDirectory(Oid fsoid)
{
	return NULL;
}



/*
 * checkpath(path)
 *
 * Runs simple validations on a path supplied to CREATE FILESPACE:
 *  - Standardizes paths via canonicalize_path()
 *  - Disallow paths with single quotes
 *  - Disallow relative paths
 *  - Disallow paths that are too long.
 *
 * We have other checks to perform, but these are the only ones that we
 * can run based only on the name without the local file system present.
 */
static void checkpath(char *path)
{
	/* Unix-ify the offered path and strip any trailing slashes */
	canonicalize_path(path);

	/* disallow quotes, else CREATE DATABASE would be at risk */
	if (strchr(path, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("filespace location \"%s\" "
						"cannot contain single quotes", path)));
	
	/*
	 * Allowing relative paths seems risky
	 *
	 * this also helps us ensure that path is not empty or whitespace
	 */
	if (!is_absolute_path(path))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("filespace locations \"%s\" "
						"must be an absolute path", path)));

	/*
	 * Check that location isn't too long. Remember that we're going to append
	 * '/<tbsoid>/<dboid>/<relid>.<nnn>'  
	 *
	 *      10 digits for each oid and the extension number,
	 *       1 digit for each slash and the '.' of the extension
	 *       = 10*4 + 4 = +44 characters
	 *
	 * Note: This may be overly conservative.  Do we ever form the whole path 
	 * explicitly?      
	 */
	if (strlen(path) >= (MAXPGPATH - 44))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("filespace location \"%s\" is too long",
						path)));
}

/*
 * checkpath2(path)
 *
 * Runs additional validations on a path supplied to CREATE FILESPACE.
 * The assumption is that the path given to us is the original path
 * that was specified in the gpfilespace command plus a path extension
 * to uniquely identify this segment.  We further assume that the original
 * path exists, but that the segment extension does not and must be created.
 * Or... if the extension path does exist then it must be an empty directory.
 *
 *  - if the path exists:
 *    - Validate that the path is a directory
 *    - Validate that the path is owned by the admin user
 *    - Validate that the path is empty
 *
 *  - if the path does not exist:
 *    - Validate that the parent directory exists
 *    - Validate that the parent is a directory.
 *    - Validate that the parent has apropriate permissions
 *
 * Note: Passing these checks does not guarantee that everything is good.
 * In particular we have not checked anywhere that the paths are all 
 * unique on a given host.  We omit this only because this is a difficult
 * test when we don't have metadata about what segments are on the same host.
 * 
 * If there is a conflict we should see it when we actually try to claim
 * the directories for the segments.
 *
 * Note: May need to add additional checks that there is not a pending
 *   background delete on this directory location?
 */
static void checkpath2(char *path)
{
	DIR           *dir;
	struct dirent *de;
	struct stat    st;

	/* Does the path exist? */
	if (stat(path, &st) >= 0)
	{
		/* Is it actually a directory? */
		if (! S_ISDIR(st.st_mode))
			ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("%s : Not a directory", path)));

		/* The directory must be owned by gpadmin user account */
		if (st.st_uid != geteuid())
			ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("%s : Permission denied", path)));

		/* Check that the directory is empty */
		if ((dir = AllocateDir(path)) == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("%s : Error reading directory", path)));
		while ((de = ReadDir(dir, path)) != NULL)
		{
			if (strcmp(de->d_name, ".") == 0 ||
				strcmp(de->d_name, "..") == 0)
				continue;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("%s : Directory not empty", path)));
		}
		FreeDir(dir);
	}
	else
	{
		/* path does not exist, run checks on parent */
		char *parentdir = pstrdup(path);
		get_parent_directory(parentdir);

		/* Does the parent directory exist? */
		if (stat(parentdir, &st) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("%s : No such file or directory", parentdir)));
		
		/* Is it actually a directory? */
		if (! S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("%s : Not a directory", parentdir)));

		/* Check write permissions */
		if (access(parentdir, W_OK) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("%s: No write permissions", parentdir)));
	}
}
