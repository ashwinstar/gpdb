/*-------------------------------------------------------------------------
 *
 * index.h
 *	  prototypes for catalog/index.c.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/index.h,v 1.71 2006/08/25 04:06:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_H
#define INDEX_H

#include "access/relscan.h"     /* Relation, Snapshot */
#include "executor/tuptable.h"  /* TupTableSlot */

struct EState;                  /* #include "nodes/execnodes.h" */

#define DEFAULT_INDEX_TYPE	"btree"

/* ----------------
 *	  IndexInfo information
 *
 *		this struct holds the information needed to construct new index
 *		entries for a particular index.  Used for both index_build and
 *		retail creation of index entries.
 *
 *		NumIndexAttrs		number of columns in this index
 *		KeyAttrNumbers		underlying-rel attribute numbers used as keys
 *							(zeroes indicate expressions)
 *		Expressions			expr trees for expression entries, or NIL if none
 *		ExpressionsState	exec state for expressions, or NIL if none
 *		Predicate			partial-index predicate, or NIL if none
 *		PredicateState		exec state for predicate, or NIL if none
 *		Unique				is it a unique index?
 *		Concurrent			are we doing a concurrent index build?
 *
 *      CDB: Moved this declaration from nodes/execnodes.h into catalog/index.h
 * ----------------
 */
typedef struct IndexInfo
{
	NodeTag		type;
	int			ii_NumIndexAttrs;
	AttrNumber	ii_KeyAttrNumbers[INDEX_MAX_KEYS];
	List	   *ii_Expressions; /* list of Expr */
	List	   *ii_ExpressionsState;	/* list of ExprState */
	List	   *ii_Predicate;	/* list of Expr */
	List	   *ii_PredicateState;		/* list of ExprState */
	bool		ii_Unique;
	bool		ii_Concurrent;

	void       *opaque; /* Additional info needed by index creation.
						   Only used for bitmap index to store oids that
						   are needed for lov heap and lov index. */

} IndexInfo;

typedef struct IndexInfoOpaque
{
	Oid        comptypeOid; /* the complex type oid for the lov heap. */
	Oid        heapOid;  /* Oid for the lov heap in the bitmap index. */
	Oid        indexOid; /* Oid for the lov index in the bitmap index. */
} IndexInfoOpaque;

/* Typedef for callback function for IndexBuildHeapScan */
typedef void (*IndexBuildCallback) (Relation index,
												HeapTuple htup,
												Datum *values,
												bool *isnull,
												bool tupleIsAlive,
												void *state);


extern Oid index_create(Oid heapRelationId,
			 const char *indexRelationName,
			 Oid indexRelationId,
			 struct IndexInfo *indexInfo,
			 Oid accessMethodObjectId,
			 Oid tableSpaceId,
			 Oid *classObjectId,
			 Datum reloptions,
			 bool isprimary,
			 bool isconstraint,
			 bool allow_system_table_mods,
			 bool skip_build,
			 bool concurrent);

extern void index_drop(Oid indexId);

extern struct IndexInfo *BuildIndexInfo(Relation index);

extern void FormIndexDatum(struct IndexInfo *indexInfo,
			   TupleTableSlot *slot,
			   struct EState *estate,
			   Datum *values,
			   bool *isnull);

extern Oid setNewRelfilenode(Relation relation);

extern Oid setNewRelfilenodeToOid(Relation relation, Oid newrelfilenode);

extern void index_build(Relation heapRelation,
			Relation indexRelation,
			struct IndexInfo *indexInfo,
			bool isprimary);

extern double IndexBuildHeapScan(Relation heapRelation,
				   Relation indexRelation,
				   struct IndexInfo *indexInfo,
				   IndexBuildCallback callback,
				   void *callback_state);

extern void validate_index(Oid heapId, Oid indexId, Snapshot snapshot);

extern void reindex_index(Oid indexId);
extern bool reindex_relation(Oid relid, bool toast_too);

#endif   /* INDEX_H */
