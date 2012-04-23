/*-------------------------------------------------------------------------
 *
 * pg_largeobject.c
 *	  routines to support manipulation of the pg_largeobject relation
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_largeobject.c,v 1.32 2009/01/01 17:23:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/catquery.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_largeobject.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/tqual.h"


/*
 * Create a large object having the given LO identifier.
 *
 * We do this by inserting an empty first page, so that the object will
 * appear to exist with size 0.  Note that the unique index will reject
 * an attempt to create a duplicate page.
 */
void
LargeObjectCreate(Oid loid)
{
	Relation	pg_largeobject;
	HeapTuple	ntup;
	Datum		values[Natts_pg_largeobject];
	bool		nulls[Natts_pg_largeobject];
	int			i;

	pg_largeobject = heap_open(LargeObjectRelationId, RowExclusiveLock);

	/*
	 * Form new tuple
	 */
	for (i = 0; i < Natts_pg_largeobject; i++)
	{
		values[i] = (Datum) 0;
		nulls[i] = false;
	}

	i = 0;
	values[i++] = ObjectIdGetDatum(loid);
	values[i++] = Int32GetDatum(0);
	values[i++] = DirectFunctionCall1(byteain,
									  CStringGetDatum(""));

	ntup = heap_form_tuple(pg_largeobject->rd_att, values, nulls);

	/*
	 * Insert it
	 */
	simple_heap_insert(pg_largeobject, ntup);

	/* Update indexes */
	CatalogUpdateIndexes(pg_largeobject, ntup);

	heap_close(pg_largeobject, RowExclusiveLock);

	heap_freetuple(ntup);
}

void
LargeObjectDrop(Oid loid)
{
	bool		found = false;

	found = (0 != caql_getcount(
					 NULL,
					 cql("DELETE FROM pg_largeobject "
						 " WHERE loid = :1 ",
						 ObjectIdGetDatum(loid))));

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", loid)));
}

bool
LargeObjectExists(Oid loid)
{
	bool		retval = false;

	/*
	 * See if we can find any tuples belonging to the specified LO
	 */

	retval = (0 != caql_getcount(
					  NULL,
					  cql("SELECT COUNT(*) FROM pg_largeobject "
						  " WHERE loid = :1 ",
						  ObjectIdGetDatum(loid))));

	return retval;
}
