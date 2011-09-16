/*
 * pg_attribute_encoding.c
 *
 * Routines to manipulation and retrieve column encoding information.
 *
 * Copyright (c) EMC, 2011
 */

#include "postgres.h"
#include "fmgr.h"

#include "access/reloptions.h"
#include "catalog/pg_attribute_encoding.h"
#include "catalog/pg_compression.h"
#include "catalog/dependency.h"
#include "cdb/cdbappendonlyam.h"
#include "cdb/cdbappendonlystoragelayer.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/*
 * Get all storage options for all user attributes of the table.
 */
StdRdOptions **
RelationGetAttributeOptions(Relation rel)
{
	HeapTuple		tuple;
	ScanKeyData		key;
	SysScanDesc		scan;
	StdRdOptions  **opts;
	Relation 		pgae = heap_open(AttributeEncodingRelationId,
									 AccessShareLock);

	opts = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(StdRdOptions *));

	ScanKeyInit(&key,
				Anum_pg_attribute_encoding_attrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	scan = systable_beginscan(pgae,
							  AttributeEncodingAttrelidIndexId,
							  true,
							  SnapshotNow,
							  1,
							  &key);

	while ((tuple = systable_getnext(scan)))
	{
		Form_pg_attribute_encoding a = 
			(Form_pg_attribute_encoding)GETSTRUCT(tuple);
		int16 attnum = a->attnum;
		Datum reloptions;
		bool isnull;

		Insist(attnum > 0 && attnum <= RelationGetNumberOfAttributes(rel));

		reloptions = heap_getattr(tuple, Anum_pg_attribute_encoding_attoptions,
								  RelationGetDescr(pgae), &isnull);
		Insist(!isnull);

		opts[attnum - 1] = (StdRdOptions *)heap_reloptions(0, reloptions, false);
	}

	systable_endscan(scan);

	heap_close(pgae, AccessShareLock);

	return opts;
}

/*
 * Return an array of compression function arrays for
 * each attribute in a table.
 *
 * Set NULL for columns without storage options.
 */
PGFunction **
RelationGetColumnCompressionFuncs(Relation rel)
{
	StdRdOptions  **opts = RelationGetAttributeOptions(rel);
	PGFunction	  **funcs = palloc0(RelationGetNumberOfAttributes(rel)
									* sizeof(PGFunction *));
	int 			i;

	for (i = 0; i < RelationGetNumberOfAttributes(rel); i++)
	{
		if (opts[i])
		{
			if (strcmp("none", opts[i]->compresstype) != 0)
			{
				PGFunction *func =
					GetCompressionImplementation(opts[i]->compresstype);

				Insist(PointerIsValid(func));

				funcs[i] = func;
			}
		}
	}
	return funcs;
}

/* Returns an array of block sizes -- one entry for each user column in rel. */
uint32 *
RelationGetColumnBlocksize(Relation rel)
{
	uint32 		   *bz = palloc(RelationGetNumberOfAttributes(rel) * sizeof(uint32));
	StdRdOptions  **opts = RelationGetAttributeOptions(rel);
	int 			i;

	for (i = 0; i < RelationGetNumberOfAttributes(rel); i++)
	{
		if (opts[i] == NULL)
			bz[i] = DEFAULT_APPENDONLY_BLOCK_SIZE;
		else
			bz[i] = opts[i]->blocksize;
	}

	return bz;
}

uint32
RelationGetRelationBlocksize(Relation rel)
{

  AppendOnlyEntry *aoentry;

  aoentry = GetAppendOnlyEntry(RelationGetRelid(rel));

  return aoentry->blocksize;

}


/*
 * Has the same signature as RelationGetAttributeCompressionFuncs() even though
 * we don't actually need the full Relation data structure. I deem consistency
 * of API more important in this case.
 */
PGFunction *
RelationGetRelationCompressionFuncs(Relation rel)
{
	AppendOnlyEntry *aoentry;
	char *comptype;
	PGFunction *compFuncs;

	aoentry = GetAppendOnlyEntry(RelationGetRelid(rel));
	comptype = aoentry->compresstype;

	if (comptype != NULL)
		compFuncs =	GetCompressionImplementation(comptype);
	else
		compFuncs = NULL;

	return compFuncs;

}

/*
 * Work horse underneath DefineRelation().
 *
 * Simply adds user specified ENCODING () clause information to
 * pg_attribute_encoding. Should be absolutely valid at this point.
 */
void
AddRelationAttributeEncodings(Relation rel, List *attr_encodings)
{
	Oid relid = RelationGetRelid(rel);
	ListCell *lc;
	Relation pgaerel;

	/* If we're in upgrade mode, shouldn't be anything to do here */
	if (gp_upgrade_mode)
	{
		Assert(attr_encodings == NIL);
		return;
	}

	pgaerel = heap_open(AttributeEncodingRelationId, RowExclusiveLock);

	foreach(lc, attr_encodings)
	{
		Datum attoptions;
		ColumnReferenceStorageDirective *c = lfirst(lc);
		Datum values[Natts_pg_attribute_encoding];
		bool nulls[Natts_pg_attribute_encoding];
		HeapTuple tuple;
		List *encoding;
		AttrNumber attnum;

		Insist(IsA(c, ColumnReferenceStorageDirective));

		attnum = get_attnum(relid, strVal(c->column));

		if (attnum == InvalidAttrNumber)
			elog(ERROR, "column \"%s\" does not exist", strVal(c->column));

		if (attnum < 0)
			elog(ERROR, "column \"%s\" is a system column", strVal(c->column));

		encoding = c->encoding;

		if (!encoding)
			continue;

		attoptions = transformRelOptions(PointerGetDatum(NULL),
										 encoding,
										 true,
										 false);

		MemSet(nulls, 0, sizeof(nulls));
		values[Anum_pg_attribute_encoding_attrelid - 1] = ObjectIdGetDatum(relid);
		values[Anum_pg_attribute_encoding_attnum - 1] = Int16GetDatum(attnum);
		values[Anum_pg_attribute_encoding_attoptions - 1] = attoptions;

		tuple = heap_form_tuple(RelationGetDescr(pgaerel), values, nulls);

		simple_heap_insert(pgaerel, tuple);

		CatalogUpdateIndexes(pgaerel, tuple);

		heap_freetuple(tuple);
	}

	heap_close(pgaerel, RowExclusiveLock);
}

static void
remove_attr_encoding(Oid relid, AttrNumber attnum, bool error_not_found)
{
	ScanKeyData skey[2];
	int			nkeys = 1;
	SysScanDesc attrencscan;
	HeapTuple	tuple;
	Relation	pgattrenc;
	bool 		found = false;
	char	   *relname;

	/* shouldn't be anything to do in upgrade mode */
	if (gp_upgrade_mode)
		return;

	relname = get_rel_name(relid);
	pgattrenc = heap_open(AttributeEncodingRelationId, RowExclusiveLock);

	ScanKeyInit(&skey[0],
				Anum_pg_attribute_encoding_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	if (attnum != InvalidAttrNumber)
	{
		ScanKeyInit(&skey[1],
					Anum_pg_attribute_encoding_attnum,
					BTEqualStrategyNumber, F_OIDEQ,
					Int16GetDatum(attnum));
		nkeys = 2;
	}

	attrencscan = systable_beginscan(pgattrenc,
									 AttributeEncodingAttrelidAttnumIndexId,
									 true, SnapshotNow, nkeys, skey);

	while ((tuple = systable_getnext(attrencscan)) != NULL)
	{
		found = true;
		simple_heap_delete(pgattrenc, &tuple->t_self);
	}

	if (!found && error_not_found)
		elog(ERROR, "cannot find pg_attribute_encoding entry for relation \"%s\"",
			 relname ? relname : "unknown");

	systable_endscan(attrencscan);
	heap_close(pgattrenc, RowExclusiveLock);
}

void
RemoveRelationAttributeEncoding(Oid relid, AttrNumber attnum)
{

	remove_attr_encoding(relid, attnum, true);
}

void
RemoveAttributeEncodingsByRelid(Oid relid)
{
	remove_attr_encoding(relid, InvalidAttrNumber, false);
}

