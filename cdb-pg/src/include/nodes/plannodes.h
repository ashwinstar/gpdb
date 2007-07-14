
/*-------------------------------------------------------------------------
 *
 * plannodes.h
 *	  definitions for query plan nodes
 *
 *
 * Portions Copyright (c) 2005-2006, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/nodes/plannodes.h,v 1.85 2006/08/02 01:59:47 joe Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_H
#define PLANNODES_H

#include "access/sdir.h"
#include "nodes/bitmapset.h"
#include "nodes/primnodes.h"
/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		Plan node
 *
 * All plan nodes "derive" from the Plan structure by having the
 * Plan structure as the first field.  This ensures that everything works
 * when nodes are cast to Plan's.  (node pointers are frequently cast to Plan*
 * when passed around generically in the executor)
 *
 * We never actually instantiate any Plan nodes; this is just the common
 * abstract superclass for all Plan-type nodes.
 * ----------------
 */
typedef struct Plan
{
	NodeTag		type;

	/*
	 * estimated execution costs for plan (see costsize.c for more info)
	 */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	/*
	 * planner's estimate of result size of this plan step
	 */
	double		plan_rows;		/* number of rows plan is expected to emit */
	int			plan_width;		/* average row width in bytes */

	/*
	 * Common structural data for all Plan types.
	 */
	List	   *targetlist;		/* target list to be computed at this node */
	List	   *qual;			/* implicitly-ANDed qual conditions */

	struct Plan *lefttree;		/* input plan tree(s) */
	struct Plan *righttree;
	List	   *initPlan;		/* Init Plan nodes (un-correlated expr
								 * subselects) */

	/*
	 * Information for management of parameter-change-driven rescanning
	 *
	 * extParam includes the paramIDs of all external PARAM_EXEC params
	 * affecting this plan node or its children.  setParam params from the
	 * node's initPlans are not included, but their extParams are.
	 *
	 * allParam includes all the extParam paramIDs, plus the IDs of local
	 * params that affect the node (i.e., the setParams of its initplans).
	 * These are _all_ the PARAM_EXEC params that affect this node.
	 */
	Bitmapset  *extParam;
	Bitmapset  *allParam;

	/*
	 * We really need in some TopPlan node to store range table and
	 * resultRelation from Query there and get rid of Query itself from
	 * Executor. Some other stuff like below could be put there, too.
	 */
	int			nParamExec;		/* Number of them in entire query. This is to
								 * get Executor know about how many PARAM_EXEC
								 * there are in query plan. */
	
	/*
	 * MPP needs to keep track of the characteristics of flow of output
	 * tuple of Plan nodes.
	 */
	Flow		*flow;			/* Flow description.  Initially NULL.
								 * Set during parallelization.
								 */
	
	/*
	 * CDB:  How should this plan tree be dispatched?  Initially this is set
	 * to DISPATCH_UNDETERMINED and, in non-root nodes, may remain so.
	 * However, in Plan nodes at the root of any separately dispatchable plan
	 * fragment, it must be set to a specific dispatch type.  This field
	 * is another candidate for a hypothetical TopPlan node as mentioned
	 * above.
	 */
	DispatchMethod dispatch;
	
	/*
	 * CDB: Now many motion nodes are there in the Plan.  How many init plans?
	 * Additional plan tree global significant only in the root node.
	 */
	 int nMotionNodes;
	 int nInitPlans;
	
	 /*
	  * CDB: This allows the slice table to accompany the plan as it
	  * moves around the executor. This is anoter plan tree global that
	  * should be non-NULL only in the top node of a dispatchable tree.
	  * It could (and should) move to a TopPlan node if we ever do that.
	  *
	  * Currently, the slice table should not be installed on the QD.
	  * Rather is it shipped to QEs as a separate parameter to MPPEXEC.
	  * The implementation of MPPEXEC, which runs on the QEs, installs
	  * the slice table in the plan as required there.
	  */
	Node *sliceTable;
} Plan;

/* ----------------
 *	these are are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlan(node)			(((Plan *)(node))->righttree)
#define outerPlan(node)			(((Plan *)(node))->lefttree)


/* ----------------
 *	 Result node -
 *		If no outer plan, evaluate a variable-free targetlist.
 *		If outer plan, return tuples from outer plan (after a level of
 *		projection as shown by targetlist).
 *
 * If resconstantqual isn't NULL, it represents a one-time qualification
 * test (i.e., one that doesn't depend on any variables from the outer plan,
 * so needs to be evaluated only once).
 * ----------------
 */
typedef struct Result
{
	Plan		plan;
	Node	   *resconstantqual;
} Result;

/* ----------------
 *	 Append node -
 *		Generate the concatenation of the results of sub-plans.
 *
 * Append nodes are sometimes used to switch between several result relations
 * (when the target of an UPDATE or DELETE is an inheritance set).	Such a
 * node will have isTarget true.  The Append executor is then responsible
 * for updating the executor state to point at the correct target relation
 * whenever it switches subplans.
 * ----------------
 */
typedef struct Append
{
	Plan		plan;
	List	   *appendplans;
	bool		isTarget;
} Append;

/* ----------------
 *	 BitmapAnd node -
 *		Generate the intersection of the results of sub-plans.
 *
 * The subplans must be of types that yield tuple bitmaps.	The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * ----------------
 */
typedef struct BitmapAnd
{
	Plan		plan;
	List	   *bitmapplans;
} BitmapAnd;

/* ----------------
 *	 BitmapOr node -
 *		Generate the union of the results of sub-plans.
 *
 * The subplans must be of types that yield tuple bitmaps.	The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * ----------------
 */
typedef struct BitmapOr
{
	Plan		plan;
	List	   *bitmapplans;
} BitmapOr;

/*
 * ==========
 * Scan nodes
 * ==========
 */
typedef struct Scan
{
	Plan		plan;
	Index		scanrelid;		/* relid is index into the range table */
} Scan;

/* ----------------
 *		sequential scan node
 * ----------------
 */
typedef Scan SeqScan;

/* ----------------
 *		index scan node
 *
 * indexqualorig is an implicitly-ANDed list of index qual expressions, each
 * in the same form it appeared in the query WHERE condition.  Each should
 * be of the form (indexkey OP comparisonval) or (comparisonval OP indexkey).
 * The indexkey is a Var or expression referencing column(s) of the index's
 * base table.	The comparisonval might be any expression, but it won't use
 * any columns of the base table.
 *
 * indexqual has the same form, but the expressions have been commuted if
 * necessary to put the indexkeys on the left, and the indexkeys are replaced
 * by Var nodes identifying the index columns (varattno is the index column
 * position, not the base table's column, even though varno is for the base
 * table).	This is a bit hokey ... would be cleaner to use a special-purpose
 * node type that could not be mistaken for a regular Var.	But it will do
 * for now.
 *
 * indexstrategy and indexsubtype are lists corresponding one-to-one with
 * indexqual; they give information about the indexable operators that appear
 * at the top of each indexqual.
 * ----------------
 */
typedef struct IndexScan
{
	Scan		scan;
	Oid			indexid;		/* OID of index to scan */
	List	   *indexqual;		/* list of index quals (OpExprs) */
	List	   *indexqualorig;	/* the same in original form */
	List	   *indexstrategy;	/* integer list of strategy numbers */
	List	   *indexsubtype;	/* OID list of strategy subtypes */
	ScanDirection indexorderdir;	/* forward or backward or don't care */
} IndexScan;

/* ----------------
 *		bitmap index scan node
 *
 * BitmapIndexScan delivers a bitmap of potential tuple locations;
 * it does not access the heap itself.	The bitmap is used by an
 * ancestor BitmapHeapScan node, possibly after passing through
 * intermediate BitmapAnd and/or BitmapOr nodes to combine it with
 * the results of other BitmapIndexScans.
 *
 * The fields have the same meanings as for IndexScan, except we don't
 * store a direction flag because direction is uninteresting.
 *
 * In a BitmapIndexScan plan node, the targetlist and qual fields are
 * not used and are always NIL.  The indexqualorig field is unused at
 * run time too, but is saved for the benefit of EXPLAIN.
 * ----------------
 */
typedef struct BitmapIndexScan
{
	Scan		scan;
	Oid			indexid;		/* OID of index to scan */
	List	   *indexqual;		/* list of index quals (OpExprs) */
	List	   *indexqualorig;	/* the same in original form */
	List	   *indexstrategy;	/* integer list of strategy numbers */
	List	   *indexsubtype;	/* OID list of strategy subtypes */
} BitmapIndexScan;

/* ----------------
 *		bitmap sequential scan node
 *
 * This needs a copy of the qual conditions being used by the input index
 * scans because there are various cases where we need to recheck the quals;
 * for example, when the bitmap is lossy about the specific rows on a page
 * that meet the index condition.
 * ----------------
 */
typedef struct BitmapHeapScan
{
	Scan		scan;
	List	   *bitmapqualorig; /* index quals, in standard expr form */
} BitmapHeapScan;

/* ----------------
 *		tid scan node
 *
 * tidquals is an implicitly OR'ed list of qual expressions of the form
 * "CTID = pseudoconstant" or "CTID = ANY(pseudoconstant_array)".
 * ----------------
 */
typedef struct TidScan
{
	Scan		scan;
	List	   *tidquals;		/* qual(s) involving CTID = something */
} TidScan;

/* ----------------
 *		subquery scan node
 *
 * SubqueryScan is for scanning the output of a sub-query in the range table.
 * We need a special plan node above the sub-query's plan as a place to switch
 * execution contexts.	Although we are not scanning a physical relation,
 * we make this a descendant of Scan anyway for code-sharing purposes.
 *
 * Note: we store the sub-plan in the type-specific subplan field, not in
 * the generic lefttree field as you might expect.	This is because we do
 * not want plan-tree-traversal routines to recurse into the subplan without
 * knowing that they are changing Query contexts.
 * ----------------
 */
typedef struct SubqueryScan
{
	Scan		scan;
	Plan	   *subplan;
} SubqueryScan;

/* ----------------
 *		FunctionScan node
 * ----------------
 */
typedef struct FunctionScan
{
	Scan		scan;
	/* no other fields needed at present */
} FunctionScan;

/* ----------------
 *		ValuesScan node
 * ----------------
 */
typedef struct ValuesScan
{
	Scan		scan;
	/* no other fields needed at present */
} ValuesScan;

/* ----------------
* External Scan node
* 
* Field scan.scanrelid is the index of the external relation for
* this node.
*
* Field filenames is a list of N string node pointers (or NULL)
* where N is number of segments in the array. The pointer in
* position I is NULL or points to the string node containing the
* file name for segment I.
* ----------------
*/
typedef struct ExternalScan
{
	Scan		scan;
	List		*uriList;   /* data uri or null for each segment  */
	List		*fmtOpts;   /* data format options                */
	bool		isText;     /* true for text false for csv        */
	uint32      scancounter;	/* counter incr per scan node created */

} ExternalScan;

/*
 * ==========
 * Join nodes
 * ==========
 */

/* ----------------
 *		Join node
 *
 * jointype:	rule for joining tuples from left and right subtrees
 * joinqual:	qual conditions that came from JOIN/ON or JOIN/USING
 *				(plan.qual contains conditions that came from WHERE)
 *
 * When jointype is INNER, joinqual and plan.qual are semantically
 * interchangeable.  For OUTER jointypes, the two are *not* interchangeable;
 * only joinqual is used to determine whether a match has been found for
 * the purpose of deciding whether to generate null-extended tuples.
 * (But plan.qual is still applied before actually returning a tuple.)
 * For an outer join, only joinquals are allowed to be used as the merge
 * or hash condition of a merge or hash join.
 * ----------------
 */
typedef struct Join
{
	Plan		plan;
	JoinType	jointype;
	List		*joinqual;		/* JOIN quals (in addition to plan.qual) */

	bool		prefetch_inner; /* to avoid deadlock in MPP */
} Join;

/* ----------------
 *		nest loop join node
 * ----------------
 */
typedef struct NestLoop
{
	Join		join;

    bool		outernotreferencedbyinner;  /* true => inner has no OUTER Var */
	bool		shared_outer;
} NestLoop;

/* ----------------
 *		merge join node
 * ----------------
 */
typedef struct MergeJoin
{
	Join		join;
	List	   *mergeclauses;
} MergeJoin;

/* ----------------
 *		hash join (probe) node
 * ----------------
 */
typedef struct HashJoin
{
	Join		join;
	List	   *hashclauses;
} HashJoin;

/* ----------------
 *		adaptive nest loop node: not a proper join node, has joins as child plans
 * ----------------
 */
typedef struct AdaptiveNestLoop
{
	Plan		plan;

	/* Plan-B structures */
	int			nl_strategy_threshold; /* how many outer tuples before we change join strategy */
} AdaptiveNestLoop;

/*
 * Share type of sharing a node.
 */
typedef enum ShareType
{
	SHARE_NOTSHARED, 
	SHARE_MATERIAL,          	/* Sharing a material node */
	SHARE_MATERIAL_XSLICE,		/* Sharing a material node, across slice */
	SHARE_SORT,					/* Sharing a sort */
	SHARE_SORT_XSLICE,			/* Sharing a sort, across slice */
	/* Other types maybe added later, like sharing a hash */
} ShareType;

#define SHARE_ID_NOT_SHARED (-1)
#define SHARE_ID_NOT_ASSIGNED (-2)

extern int get_plan_share_id(Plan *p);
extern void set_plan_share_id(Plan *p, int share_id);
extern ShareType get_plan_share_type (Plan *p);
extern void set_plan_share_type(Plan *p, ShareType st);
extern void set_plan_share_type_xslice(Plan *p);
extern int get_plan_driver_slice(Plan *p);
extern void set_plan_driver_slice(Plan *P, int slice);

/* ----------------
 *		shareinputscan node
 * ----------------
 */
typedef struct ShareInputScan
{
	Plan 		plan; /* The ShareInput */

	ShareType 	share_type; 
	int 		share_id;
} ShareInputScan;

/* ----------------
 * 		Material node 
 * ----------------
 */
typedef struct Material
{
	Plan		plan;
    bool        cdb_strict;

	/* Material can be shared */
	ShareType 	share_type;
	int 		share_id;
	int         driver_slice;
} Material;


/* ----------------
 *		sort node
 * ----------------
 */
typedef struct Sort
{
	Plan		plan;
	int			numCols;		/* number of sort-key columns */
	AttrNumber *sortColIdx;		/* their indexes in the target list */
	Oid		   *sortOperators;	/* OIDs of operators to sort them by */
    /* CDB */ /* add limit node, distinct */
	Node	   *limitOffset;	/* OFFSET parameter, or NULL if none */
	Node	   *limitCount;		/* COUNT parameter, or NULL if none */
	bool		noduplicates;   /* TRUE if sort should discard duplicates */

	/* Sort node can be shared */
	ShareType 	share_type;
	int 		share_id;
	int 		driver_slice;
} Sort;

/* ---------------
 *	 group node -
 *		Used for queries with GROUP BY (but no aggregates) specified.
 *		The input must be presorted according to the grouping columns.
 * ---------------
 */
typedef struct Group
{
	Plan		plan;
	int			numCols;		/* number of grouping columns */
	AttrNumber *grpColIdx;		/* their indexes in the target list */

	/*
	 * The following are used by ROLLUP.
	 */

	/*
	 * The number of null grouping columns for this node.
	 */
	int         numNullCols;

    /*
	 * Indicate the GROUPING value of input tuples for this Group node.
	 * Refer to struct Agg for details.
	 *
	 * If this is not for a rollup, this value is 0.
	 */
	uint64 inputGrouping;

	/* The value of GROUPING for this rollup-aware node. */
	uint64 grouping;

	/*
	 * Indicate if input tuples contain values for GROUPING column.
	 */
	bool inputHasGrouping;

	/*
	 * How many times the aggregates in this rollup level will be output
	 * for a given query. Used only for ROLLUP queries.
	 */
	int         rollupGSTimes;

	/*
	 * Indicate if this Group node is the last one in a rollup.
	 *
	 * We need to output tuples for duplicate grouping sets at the final
	 * Group node. Unlike Agg nodes, we do not have a way to tell if
	 * a Group node is the final one. We add this value for this purpose.
	 */
	bool        lastGroup;
} Group;

/* ---------------
 *		aggregate node
 *
 * An Agg node implements plain or grouped aggregation.  For grouped
 * aggregation, we can work with presorted input or unsorted input;
 * the latter strategy uses an internal hashtable.
 *
 * Notice the lack of any direct info about the aggregate functions to be
 * computed.  They are found by scanning the node's tlist and quals during
 * executor startup.  (It is possible that there are no aggregate functions;
 * this could happen if they get optimized away by constant-folding, or if
 * we are using the Agg node to implement hash-based grouping.)
 * ---------------
 */
typedef enum AggStrategy
{
	AGG_PLAIN,					/* simple agg across all input rows */
	AGG_SORTED,					/* grouped agg, input must be sorted */
	AGG_HASHED					/* grouped agg, use internal hashtable */
} AggStrategy;

typedef struct Agg
{
	Plan		plan;
	AggStrategy aggstrategy;
	int			numCols;		/* number of grouping columns */
	AttrNumber *grpColIdx;		/* their indexes in the target list */
	long		numGroups;		/* estimated number of groups in input */
	int			transSpace;		/* est storage per group for byRef transition values */

	/*
	 * The following is used by ROLLUP.
	 */

	/*
	 * The number of grouping columns for this node whose values should be null for
	 * this Agg node.
	 */
	int         numNullCols;

    /*
	 * Indicate the GROUPING value of input tuples for this Agg node.
	 * For example of ROLLUP(a,b,c), there are four Agg nodes:
	 *
	 *   Agg(a,b,c) ==> Agg(a,b) ==> Agg(a) ==> Agg()
	 *
	 * The GROUPING value of input tuples for Agg(a,b,c) is 0, and the values
	 * for Agg(a,b), Agg(a), Agg() are 0, 1, 3, respectively.
	 *
	 * We also use the value "-1" to indicate an Agg node is the final
	 * one that brings back all rollup results from different segments. This final
	 * Agg node is very similar to the non-rollup Agg node, except that we need
	 * a way to know this to properly set GROUPING value during execution.
	 *
	 * For a non-rollup Agg node, this value is 0.
	 */
	uint64 inputGrouping;

	/* The value of GROUPING for this rollup-aware node. */
	uint64 grouping;

	/*
	 * Indicate if input tuples contain values for GROUPING column.
	 *
	 * This is used to determine if the node that generates inputs
	 * for this Agg node is also an Agg node. That is, this Agg node
	 * is one of the list of Agg nodes for a ROLLUP. One exception is
	 * the first Agg node in the list, whose inputs do not have a
	 * GROUPING column.
	 */
	bool inputHasGrouping;

	/*
	 * How many times the aggregates in this rollup level will be output
	 * for a given query. Used only for ROLLUP queries.
	 */
	int         rollupGSTimes;
} Agg;

/* ---------------
 *		window node
 *
 * A Window node implements window functions over zero or more
 * ordering/framing specifications within a partition specification on
 * appropriately ordered input.
 * 
 * For example, if there are window functions
 * 
 *   over (partition by a,b orderby c) and 
 *   over (partition by a,b order by c,d,e)
 * 
 * then the input (outer plan) of the window node will be sorted by
 * (a,b,c,d,e) -- the common partition key (a,b) and the partial
 * ordering keys (c) and (d,e).
 * 
 * A Window node contains no direct information about the window
 * functions it computes.  Those functions are found by scanning
 * the node's targetlist for WindowRef nodes during executor startup.
 * There need not be any, but there's no good reason for the planner
 * to construct a Window node without at least one WindowRef.
 * 
 * A WindowRef is related to its Window node by the fact that it is
 * contained by it.  It may also be related to a particular WindowKey
 * node in the windowKeys list.  The WindowRef field winlevel is the
 * position (counting from 0) of its WindowKey.  If winlevel equals
 * the length of the windowKeys list, than it has not WindowKey an
 * applied to the unordered partition as a whole.
 * 
 * A WindowKey specifies a partial ordering key.  It may optionally
 * specify framing.  The actual ordering key at a given level is the
 * concatenation of the partial ordering keys prior to and including
 * that level.
 * 
 * For example, the ordering key for the WindowKey in position 2 of
 * the windowKeys list is the concatenation of the partial keys found
 * in positions 0 through 2. *
 * ---------------
 */
typedef struct Window
{
	Plan			plan;
	int				numPartCols;	/* number of partitioning columns */
	AttrNumber	   *partColIdx;		/* their indexes in the target list
									   of the window's outer plan.  */
	List	       *windowKeys;		/* list of WindowKey nodes */
} Window;


/* ----------------
 *		unique node
 * ----------------
 */
typedef struct Unique
{
	Plan		plan;
	int			numCols;		/* number of columns to check for uniqueness */
	AttrNumber *uniqColIdx;		/* indexes into the target list */
} Unique;

/* ----------------
 *		hash build node
 * ----------------
 */
typedef struct Hash
{
	Plan		plan;
	/* all other info is in the parent HashJoin node */
} Hash;

/* ----------------
 *		setop node
 * ----------------
 */
typedef enum SetOpCmd
{
	SETOPCMD_INTERSECT,
	SETOPCMD_INTERSECT_ALL,
	SETOPCMD_EXCEPT,
	SETOPCMD_EXCEPT_ALL
} SetOpCmd;

typedef struct SetOp
{
	Plan		plan;
	SetOpCmd	cmd;			/* what to do */
	int			numCols;		/* number of columns to check for
								 * duplicate-ness */
	AttrNumber *dupColIdx;		/* indexes into the target list */
	AttrNumber	flagColIdx;
} SetOp;

/* ----------------
 *		limit node
 *
 * Note: as of Postgres 8.2, the offset and count expressions are expected
 * to yield int8, rather than int4 as before.
 * ----------------
 */
typedef struct Limit
{
	Plan		plan;
	Node	   *limitOffset;	/* OFFSET parameter, or NULL if none */
	Node	   *limitCount;		/* COUNT parameter, or NULL if none */
} Limit;

/* -------------------------
 *		motion node structs
 * -------------------------
 */
typedef enum MotionType
{
	MOTIONTYPE_HASH,		/* Use hashing to select a segindex destination */
	MOTIONTYPE_FIXED,		/* Send tuples to a fixed set of segindexes */
} MotionType;

/*
 * Motion Node
 *
 */
typedef struct Motion
{
	Plan		plan;

	MotionType  motionType;
	bool		sendToSecondary;	/* if true, secondary instances will get sent tuples as well as primary */
	bool		sendSorted;			/* if true, output should be sorted */
	int			motionID;			/* required by AMS  */

	/* For Hash */
	int			numHashSegs;		/* number of segindexes in hashSegIdx array */	
	int			*hashSegIdx;		/* array of segindex values to hash to */
	List		*hashExpr;			/* list of hash expressions */
	List		*hashDataTypes;	    /* list of hash expr data type oids */

	/* For Fixed */
	int 	  	numOutputSegs;		/* number of seg indexes in outputSegIdx array, 0 for broadcast */
	int 	 	*outputSegIdx; 	 	/* array of output segindexes */


    /* The sending gang */
    int			numInputSegs;		/* number of sending processes */
	int        *inputSegIdx;		/* array of input segids (NULL=>default) */

	List       *sliceEntries;       /* for each output slice */

	/* The following field is only used when sendSorted == true */
	int			numSortCols;		/* number of sort key columns */
	AttrNumber	*sortColIdx;		/* their indexes in target list */
	Oid			*sortOperators;		/* OID of operators to sort them by */
} Motion;

#endif   /* PLANNODES_H */
