/*-------------------------------------------------------------------------
 *
 * execnodes.h
 *	  definitions for executor state nodes
 *
 *
 * Portions Copyright (c) 2005-2007, greenplum inc
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/nodes/execnodes.h,v 1.161.2.1 2006/12/26 21:37:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECNODES_H
#define EXECNODES_H

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "access/relscan.h"
#include "cdb/cdbhash.h"
#include "fmgr.h"                       /* ExprDoneCond, SetFunctionReturnMode */
#include "lib/dllist.h"
#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "nodes/tidbitmap.h"
#include "utils/hsearch.h"
#include "utils/tuplesort.h"

struct CdbDispatchResults;              /* in cdbdispatchresult.h */
struct CdbExplain_ShowStatCtx;          /* private, in "cdb/cdbexplain.c" */
struct ChunkTransportState;             /* #include "cdb/cdbinterconnect.h" */
struct IndexInfo;                       /* #include "catalog/index.h" */
struct StringInfoData;                  /* #include "lib/stringinfo.h" */
struct Tuplestorestate;                 /* #include "utils/tuplestore.h" */
struct TuplestorePos;                   /* #include "utils/tuplestore.h" */

/* ----------------
 *	  IndexInfo information
 *
 *		CDB: Moved declaration into "catalog/index.h" from "nodes/execnodes.h"
 * ----------------
 */

/* ----------------
 *	  ExprContext_CB
 *
 *		List of callbacks to be called at ExprContext shutdown.
 * ----------------
 */
typedef void (*ExprContextCallbackFunction) (Datum arg);

typedef struct ExprContext_CB
{
	struct ExprContext_CB *next;
	ExprContextCallbackFunction function;
	Datum		arg;
} ExprContext_CB;

/* ----------------
 *	  ExprContext
 *
 *		This class holds the "current context" information
 *		needed to evaluate expressions for doing tuple qualifications
 *		and tuple projections.	For example, if an expression refers
 *		to an attribute in the current inner tuple then we need to know
 *		what the current inner tuple is and so we look at the expression
 *		context.
 *
 *	There are two memory contexts associated with an ExprContext:
 *	* ecxt_per_query_memory is a query-lifespan context, typically the same
 *	  context the ExprContext node itself is allocated in.	This context
 *	  can be used for purposes such as storing function call cache info.
 *	* ecxt_per_tuple_memory is a short-term context for expression results.
 *	  As the name suggests, it will typically be reset once per tuple,
 *	  before we begin to evaluate expressions for that tuple.  Each
 *	  ExprContext normally has its very own per-tuple memory context.
 *
 *	CurrentMemoryContext should be set to ecxt_per_tuple_memory before
 *	calling ExecEvalExpr() --- see ExecEvalExprSwitchContext().
 * ----------------
 */
typedef struct ExprContext
{
	NodeTag		type;

	/* Tuples that Var nodes in expression may refer to */
	TupleTableSlot *ecxt_scantuple;
	TupleTableSlot *ecxt_innertuple;
	TupleTableSlot *ecxt_outertuple;

	/* Memory contexts for expression evaluation --- see notes above */
	MemoryContext ecxt_per_query_memory;
	MemoryContext ecxt_per_tuple_memory;

	/* Values to substitute for Param nodes in expression */
	ParamExecData *ecxt_param_exec_vals;		/* for PARAM_EXEC params */
	ParamListInfo ecxt_param_list_info; /* for other param types */

	/* Values to substitute for Aggref/WindowRef nodes in expression */
	Datum	   *ecxt_aggvalues; /* precomputed values for ref nodes */
	bool	   *ecxt_aggnulls;	/* null flags for ref nodes */

	/* Value to substitute for CaseTestExpr nodes in expression */
	Datum		caseValue_datum;
	bool		caseValue_isNull;

	/* Value to substitute for CoerceToDomainValue nodes in expression */
	Datum		domainValue_datum;
	bool		domainValue_isNull;

	/* Link to containing EState (NULL if a standalone ExprContext) */
	struct EState *ecxt_estate;

	/* Functions to call back when ExprContext is shut down */
	ExprContext_CB *ecxt_callbacks;

	/* Representing the final grouping and group_id for a tuple
	 * in a grouping extension query. */
	uint64      grouping;
	uint32      group_id;
} ExprContext;

/* ----------------
 *		Support for functions that might return sets (multiple rows)
 *
 *      CDB: Moved these declarations into "fmgr.h" from "nodes/execnodes.h"...
 *          enum ExprDoneCond;
 *          enum SetFunctionReturnMode;
 *          struct ReturnSetInfo;
 * ----------------
 */

/* ----------------
 *		ProjectionInfo node information
 *
 *		This is all the information needed to perform projections ---
 *		that is, form new tuples by evaluation of targetlist expressions.
 *		Nodes which need to do projections create one of these.
 *
 *		ExecProject() evaluates the tlist, forms a tuple, and stores it
 *		in the given slot.	Note that the result will be a "virtual" tuple
 *		unless ExecMaterializeSlot() is then called to force it to be
 *		converted to a physical tuple.	The slot must have a tupledesc
 *		that matches the output of the tlist!
 *
 *		The planner very often produces tlists that consist entirely of
 *		simple Var references (lower levels of a plan tree almost always
 *		look like that).  So we have an optimization to handle that case
 *		with minimum overhead.
 *
 *		targetlist		target list for projection
 *		exprContext		expression context in which to evaluate targetlist
 *		slot			slot to place projection result in
 *		itemIsDone		workspace for ExecProject
 *		isVarList		TRUE if simple-Var-list optimization applies
 *		varSlotOffsets	array indicating which slot each simple Var is from
 *		varNumbers		array indicating attr numbers of simple Vars
 *		lastInnerVar	highest attnum from inner tuple slot (0 if none)
 *		lastOuterVar	highest attnum from outer tuple slot (0 if none)
 *		lastScanVar		highest attnum from scan tuple slot (0 if none)
 * ----------------
 */
typedef struct ProjectionInfo
{
	NodeTag		type;
	List	   *pi_targetlist;
	ExprContext *pi_exprContext;
	TupleTableSlot *pi_slot;
	ExprDoneCond *pi_itemIsDone;
	bool		pi_isVarList;
	int		   *pi_varSlotOffsets;
	int		   *pi_varNumbers;
	int			pi_lastInnerVar;
	int			pi_lastOuterVar;
	int			pi_lastScanVar;
} ProjectionInfo;

/* ----------------
 *	  JunkFilter
 *
 *	  This class is used to store information regarding junk attributes.
 *	  A junk attribute is an attribute in a tuple that is needed only for
 *	  storing intermediate information in the executor, and does not belong
 *	  in emitted tuples.  For example, when we do an UPDATE query,
 *	  the planner adds a "junk" entry to the targetlist so that the tuples
 *	  returned to ExecutePlan() contain an extra attribute: the ctid of
 *	  the tuple to be updated.	This is needed to do the update, but we
 *	  don't want the ctid to be part of the stored new tuple!  So, we
 *	  apply a "junk filter" to remove the junk attributes and form the
 *	  real output tuple.
 *
 *	  targetList:		the original target list (including junk attributes).
 *	  cleanTupType:		the tuple descriptor for the "clean" tuple (with
 *						junk attributes removed).
 *	  cleanMap:			A map with the correspondence between the non-junk
 *						attribute numbers of the "original" tuple and the
 *						attribute numbers of the "clean" tuple.
 *	  resultSlot:		tuple slot used to hold cleaned tuple.
 * ----------------
 */
typedef struct JunkFilter
{
	NodeTag		type;
	List	   *jf_targetList;
	TupleDesc	jf_cleanTupType;
	AttrNumber *jf_cleanMap;
	TupleTableSlot *jf_resultSlot;
} JunkFilter;



/* ----------------
 *	  ResultRelInfo information
 *
 *		Whenever we update an existing relation, we have to
 *		update indices on the relation, and perhaps also fire triggers.
 *		The ResultRelInfo class is used to hold all the information needed
 *		about a result relation, including indices.. -cim 10/15/89
 *
 *		RangeTableIndex			result relation's range table index
 *		RelationDesc			relation descriptor for result relation
 *		NumIndices				# of indices existing on result relation
 *		IndexRelationDescs		array of relation descriptors for indices
 *		IndexRelationInfo		array of key/attr info for indices
 *		TrigDesc				triggers to be fired, if any
 *		TrigFunctions			cached lookup info for trigger functions
 *		TrigInstrument			optional runtime measurements for triggers
 *		ConstraintExprs			array of constraint-checking expr states
 *		junkFilter				for removing junk attributes from tuples
 *		projectReturning		for computing a RETURNING list
 * ----------------
 */
typedef struct ResultRelInfo
{
	NodeTag		type;
	Index		ri_RangeTableIndex;
	Relation	ri_RelationDesc;
	struct HeapInsertDesc *ri_HeapInsertDesc;
	int			ri_NumIndices;
	RelationPtr ri_IndexRelationDescs;
	struct IndexInfo  **ri_IndexRelationInfo;
	TriggerDesc *ri_TrigDesc;
	FmgrInfo   *ri_TrigFunctions;
	struct Instrumentation *ri_TrigInstrument;
	List	  **ri_ConstraintExprs;
	JunkFilter *ri_junkFilter;
	ProjectionInfo *ri_projectReturning;
} ResultRelInfo;

/* ----------------
 * MPP Worker Process information
 *
 * This structure represents the global information about a worker process.
 * It is constructed on the entry process (QD) and transmitted as part of
 * the global slice table to the involved QEs.  Note that this is an
 * immutable, fixed-size structure so it can be held in a contiguous
 * array. In the Slice node, however, it is held in a List.
 * ----------------
 */
 typedef struct CdbProcess
 {
	NodeTag type;
	
	/* These fields are established at connection (libpq) time and are
	 * available to the QD in PGconn structure associated with connected
	 * QE.  It needs to be explicitly transmitted to QE's,  however,
	 */

	char *listenerAddr; /* Interconnect listener IPv4 address, a C-string */

	int listenerPort; /* Interconnect listener port */
	int pid; /* Backend PID of the process. */
	
	/* Unclear about why we need these, however, it is no trouble to carry
	 * them.
	 */
	
	int contentid;
	int routeid;
	
 } CdbProcess;

/* ----------------
 * MPP Plan Slice information
 *
 * These structures summarize how a plan tree is sliced up into separate
 * units of execution or slices. A slice will execute on a each worker within
 * a gang of processes. Some gangs have a worker process on each of several
 * databases, others have a single worker.
 * ----------------
 */
typedef struct Slice
{
	NodeTag type;
	
	int sliceIndex;			/* The index in the global slice table of this
							 * slice.  The root slice of the main plan is
							 * always 0. Slices that have senders at their
							 * local root have a sliceIndex equal to the
							 * motionID of their sender Motion.
							 *
							 * Undefined slices should have this set to
							 * -1.
							 */
	
	int rootIndex;			/* The root slice of the slice tree of which
							 * this slice is a part.
							 */
							
	GangType gangType;		/* What kind of gang does this slice need? */
	
	int gangSize;			/* How many gang members needed?
							 *
							 * This may seem redundant, but it is set before
							 * the process lists below and used to decide how
							 * to initialize them.
							 */
							
	struct Gang *primaryGang;
	struct Gang *mirrorGang;
                            /* These are significant ONLY in the master
							 * slice table on the QD where they are used to
							 * note the gangs assigned by the the executor
							 * factory during gang allocation.
							 * They correspond to the CDBProcessLists below.
							 */
	
	int	  primary_gang_id;
	int   mirror_gang_id;
							/*
							 * So the Dispatch Agent can know what gang we are
							 * talking about
							 */
							
	int 	parentIndex; 	/* the index of parent in global slice table (origin 0)
							 * or -1 if this is root slice.
							 */
							
	List *children;			/* An integer list of indices in the global slice
							 * table (origin  0) of the child slices of
							 * this slice, or -1 if this is a leaf slice.
							 * A child slice corresponds to a receiving
							 * motion in this slice.
							 */
	
	List *primaryProcesses;	/* A list of CDBProcess nodes corresponding to
							 * the worker processes allocated to implement
							 * this plan slice.
							 *
							 * The number of processes must agree with the
							 * the plan slice to be implemented.  In MPP 2
							 * the possibilities are 1 for a slice that
							 * operates on a single stream of tuples (e.g.,
							 * the receiver of a fixed motion, a 1-gang), or
							 * the number of segments (e.g., a parallel
							 * operation or the receiver of the results of a
							 * parallel operation, an N-gang).
							 *
							 * The processes of an N-gang must be in hash
							 * value order -- a hash value of H used to direct
							 * an input tuple to this slice will direct it to
							 * the H+1st process in the list.
							 */
							
	List *mirrorProcesses;	/* A list of CDBProcess nodes corresponding to
							 * the mirrors of the primaryProcesses list entries,
							 * or NIL if no mirroring is needed in this slice.
							 */
} Slice;

/* The SliceTable is a list of Slice structures organized into root slices
 * and motion slices as follows:
 *
 * Slice 0 is the root slice of plan as a whole.
 * Slices 1 through nMotion are motion slices with a sending motion at
 *  the root of the slice.
 * Slices nMotion+1 and on are root slices of initPlans.
 *
 * There may be unused slices in case the plan contains subplans that
 * are  not initPlans.  (This won't happen unless MPP decides to support
 * subplans similarly to PostgreSQL, which isn't the current plan.)
 *
 */
typedef struct SliceTable
{
	NodeTag type;
	
	int nMotions;		/* The number Motion nodes in the entire plan */
	int nSubplans;		/* The number of subplan slices allocated */
	int localSlice;		/* Index of the slice to execute. */
 	List *slices;		/* List of slices */
	char *seqServerHost;	/* If non-null, sequence server host name. */
	int seqServerPort;		/* If seqServerHost non-null, sequence server port. */
    bool    doInstrument;   /* true => collect stats for EXPLAIN ANALYZE */
	
} SliceTable;

typedef struct ShareNodeEntry
{
	NodeTag type;

	Node *sharePlan;
	Node *shareState;
} ShareNodeEntry;

/* ----------------
 *	  EState information
 *
 * Master working state for an Executor invocation
 * ----------------
 */
typedef struct EState
{
	NodeTag		type;

	/* Basic state for all query types: */
	ScanDirection es_direction; /* current scan direction */
	Snapshot	es_snapshot;	/* time qual to use */
	Snapshot	es_crosscheck_snapshot; /* crosscheck time qual for RI */
	List	   *es_range_table; /* List of RangeTableEntrys */

	/* Info about target table for insert/update/delete queries: */
	ResultRelInfo *es_result_relations; /* array of ResultRelInfos */
	int			es_num_result_relations;		/* length of array */
	ResultRelInfo *es_result_relation_info;		/* currently active array elt */
	JunkFilter *es_junkFilter;	/* currently active junk filter */

	TupleTableSlot *es_trig_tuple_slot; /* for trigger output tuples */

	Relation	es_into_relation_descriptor;	/* for SELECT INTO */
	bool		es_into_relation_use_wal;

	/* Parameter info: */
	ParamListInfo es_param_list_info;	/* values of external params */
	ParamExecData *es_param_exec_vals;	/* values of internal params */

	/* Other working state: */
	MemoryContext es_query_cxt; /* per-query context in which EState lives */

	TupleTable	es_tupleTable;	/* Array of TupleTableSlots */

	uint64		es_processed;	/* # of tuples processed */
	Oid			es_lastoid;		/* last oid processed (by INSERT) */
	List	   *es_rowMarks;	/* not good place, but there is no other */

	bool		es_is_subquery;	/* true if subquery (es_query_cxt not mine) */

	bool		es_instrument;	/* true requests runtime instrumentation */
	bool		es_select_into; /* true if doing SELECT INTO */
	bool		es_into_oids;	/* true to generate OIDs in SELECT INTO */

	List	   *es_exprcontexts;	/* List of ExprContexts within EState */

	/*
	 * this ExprContext is for per-output-tuple operations, such as constraint
	 * checks and index-value computations.  It will be reset for each output
	 * tuple.  Note that it will be created only if needed.
	 */
	ExprContext *es_per_tuple_exprcontext;

	/* Below is to re-evaluate plan qual in READ COMMITTED mode */
	Plan	   *es_topPlan;		/* link to top of plan tree */
	struct evalPlanQual *es_evalPlanQual;		/* chain of PlanQual states */
	bool	   *es_evTupleNull; /* local array of EPQ status */
	HeapTuple  *es_evTuple;		/* shared array of EPQ substitute tuples */
	bool		es_useEvalPlan; /* evaluating EPQ tuples? */
	
	/* Additions for MPP plan slicing. */
	int es_cur_root_idx;
	int es_cur_slice_idx;
 	SliceTable *es_sliceTable;
	struct MotionState **es_slice_map;
	
	/* Data structure for node sharing */
	List **es_sharenode;

	int active_recv_id;
	void *motionlayer_context;  /* Motion Layer state */
	struct ChunkTransportState *interconnect_context; /* Interconnect state */
	
	/* MPP used resources */
	bool es_interconnect_is_setup;   /* is interconnect set-up?    */
	bool es_seqserver_is_setup;      /* is sequence server set-up? */
	
	bool es_got_eos;				/* was end-of-stream recieved? */

	bool cancelUnfinished;		/* when we're cleaning up, we need to make sure that we know it */

    /* results from qExec processes */
    struct CdbDispatchResults  *primaryResults;
    struct CdbDispatchResults  *mirrorResults;

	/* dispatch threads for this command */
	struct CdbDispatchCmdThreads  *dispatchThreads;

    /* CDB: EXPLAIN ANALYZE statistics */
    struct CdbExplain_ShowStatCtx  *showstatctx;

} EState;


/* es_rowMarks is a list of these structs: */
typedef struct ExecRowMark
{
	Relation	relation;		/* opened and RowShareLock'd relation */
	Index		rti;			/* its range table index */
	bool		forUpdate;		/* true = FOR UPDATE, false = FOR SHARE */
	bool		noWait;			/* NOWAIT option */
	char		resname[32];	/* name for its ctid junk attribute */
} ExecRowMark;


/* ----------------------------------------------------------------
 *				 Tuple Hash Tables
 *
 * All-in-memory tuple hash tables are used for a number of purposes.
 * ----------------------------------------------------------------
 */
typedef struct TupleHashEntryData *TupleHashEntry;
typedef struct TupleHashTableData *TupleHashTable;

typedef struct TupleHashEntryData
{
	/* firstTuple must be the first field in this struct! */
	MinimalTuple firstTuple;	/* copy of first tuple in this group */
	/* there may be additional data beyond the end of this struct */
} TupleHashEntryData;			/* VARIABLE LENGTH STRUCT */

typedef struct TupleHashTableData
{
	HTAB	   *hashtab;		/* underlying dynahash table */
	int			numCols;		/* number of columns in lookup key */
	AttrNumber *keyColIdx;		/* attr numbers of key columns */
	FmgrInfo   *eqfunctions;	/* lookup data for comparison functions */
	FmgrInfo   *hashfunctions;	/* lookup data for hash functions */
	MemoryContext tablecxt;		/* memory context containing table */
	MemoryContext tempcxt;		/* context for function evaluations */
	Size		entrysize;		/* actual size to make each hash entry */
	TupleTableSlot *tableslot;	/* slot for referencing table entries */
	TupleTableSlot *inputslot;	/* current input tuple's slot */
} TupleHashTableData;

typedef HASH_SEQ_STATUS TupleHashIterator;

/*
 * Use InitTupleHashIterator/TermTupleHashIterator for a read/write scan.
 * Use ResetTupleHashIterator if the table can be frozen (in this case no
 * explicit scan termination is needed).
 */
#define InitTupleHashIterator(htable, iter) \
	hash_seq_init(iter, (htable)->hashtab)
#define TermTupleHashIterator(iter) \
	hash_seq_term(iter)
#define ResetTupleHashIterator(htable, iter) \
	do { \
		hash_freeze((htable)->hashtab); \
		hash_seq_init(iter, (htable)->hashtab); \
	} while (0)
#define ScanTupleHashTable(iter) \
	((TupleHashEntry) hash_seq_search(iter))


/* ----------------------------------------------------------------
 *				 Expression State Trees
 *
 * Each executable expression tree has a parallel ExprState tree.
 *
 * Unlike PlanState, there is not an exact one-for-one correspondence between
 * ExprState node types and Expr node types.  Many Expr node types have no
 * need for node-type-specific run-time state, and so they can use plain
 * ExprState or GenericExprState as their associated ExprState node type.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExprState node
 *
 * ExprState is the common superclass for all ExprState-type nodes.
 *
 * It can also be instantiated directly for leaf Expr nodes that need no
 * local run-time state (such as Var, Const, or Param).
 *
 * To save on dispatch overhead, each ExprState node contains a function
 * pointer to the routine to execute to evaluate the node.
 * ----------------
 */

typedef struct ExprState ExprState;

typedef Datum (*ExprStateEvalFunc) (ExprState *expression,
												ExprContext *econtext,
												bool *isNull,
												ExprDoneCond *isDone);

struct ExprState
{
	NodeTag		type;
	Expr	   *expr;			/* associated Expr node */
	ExprStateEvalFunc evalfunc; /* routine to run to execute node */
};

/* ----------------
 *		GenericExprState node
 *
 * This is used for Expr node types that need no local run-time state,
 * but have one child Expr node.
 * ----------------
 */
typedef struct GenericExprState
{
	ExprState	xprstate;
	ExprState  *arg;			/* state of my child node */
} GenericExprState;

/* ----------------
 *		AggrefExprState node
 * ----------------
 */
typedef struct AggrefExprState
{
	ExprState	xprstate;
	List	   *args;			/* states of argument expressions */
	int			aggno;			/* ID number for agg within its plan node */
} AggrefExprState;

/*
 * ----------------
 *  GroupingFuncExprState node
 * ----------------
 */
typedef struct GroupingFuncExprState
{
	ExprState  xprstate;
	List	  *args;
	int        ngrpcols;   /* number of unique grouping attributes */
} GroupingFuncExprState;

/* ----------------
 *	WindowRefExprState node
 * ----------------
 */
typedef struct WindowRefExprState
{
	ExprState	xprstate;
	struct WindowState *windowstate; /* reflect parent window state */
	List	   *args;			/* states of argument expressions */
	int			refno;			/* index in window state's wrxstates list */
	int			funcno;			/* index in window state's func_state array */
	bool		isAgg;			/* aggregate-derived? */
} WindowRefExprState;

/* ----------------
 *		ArrayRefExprState node
 *
 * Note: array types can be fixed-length (typlen > 0), but only when the
 * element type is itself fixed-length.  Otherwise they are varlena structures
 * and have typlen = -1.  In any case, an array type is never pass-by-value.
 * ----------------
 */
typedef struct ArrayRefExprState
{
	ExprState	xprstate;
	List	   *refupperindexpr;	/* states for child nodes */
	List	   *reflowerindexpr;
	ExprState  *refexpr;
	ExprState  *refassgnexpr;
	int16		refattrlength;	/* typlen of array type */
	int16		refelemlength;	/* typlen of the array element type */
	bool		refelembyval;	/* is the element type pass-by-value? */
	char		refelemalign;	/* typalign of the element type */
} ArrayRefExprState;

/* ----------------
 *		FuncExprState node
 *
 * Although named for FuncExpr, this is also used for OpExpr, DistinctExpr,
 * and NullIf nodes; be careful to check what xprstate.expr is actually
 * pointing at!
 * ----------------
 */
typedef struct FuncExprState
{
	ExprState	xprstate;
	List	   *args;			/* states of argument expressions */

	/*
	 * Function manager's lookup info for the target function.  If func.fn_oid
	 * is InvalidOid, we haven't initialized it yet.
	 */
	FmgrInfo	func;

	/*
	 * We also need to store argument values across calls when evaluating a
	 * function-returning-set.
	 *
	 * setArgsValid is true when we are evaluating a set-valued function and
	 * we are in the middle of a call series; we want to pass the same
	 * argument values to the function again (and again, until it returns
	 * ExprEndResult).
	 */
	bool		setArgsValid;

	/*
	 * Flag to remember whether we found a set-valued argument to the
	 * function. This causes the function result to be a set as well. Valid
	 * only when setArgsValid is true.
	 */
	bool		setHasSetArg;	/* some argument returns a set */

	/*
	 * Flag to remember whether we have registered a shutdown callback for
	 * this FuncExprState.	We do so only if setArgsValid has been true at
	 * least once (since all the callback is for is to clear setArgsValid).
	 */
	bool		shutdown_reg;	/* a shutdown callback is registered */

	/*
	 * Current argument data for a set-valued function; contains valid data
	 * only if setArgsValid is true.
	 */
	FunctionCallInfoData setArgs;
} FuncExprState;

/* ----------------
 *		ScalarArrayOpExprState node
 *
 * This is a FuncExprState plus some additional data.
 * ----------------
 */
typedef struct ScalarArrayOpExprState
{
	FuncExprState fxprstate;
	/* Cached info about array element type */
	Oid			element_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;
} ScalarArrayOpExprState;

/* ----------------
 *		BoolExprState node
 * ----------------
 */
typedef struct BoolExprState
{
	ExprState	xprstate;
	List	   *args;			/* states of argument expression(s) */
} BoolExprState;

/* ----------------
 *		SubPlanState node
 * ----------------
 */
typedef struct SubPlanState
{
	ExprState	xprstate;
	EState	   *sub_estate;		/* subselect plan has its own EState */
	struct PlanState *planstate;	/* subselect plan's state tree */
	ExprState  *testexpr;		/* state of combining expression */
	List	   *args;			/* states of argument expression(s) */
	bool		needShutdown;	/* TRUE = need to shutdown subplan */
	HeapTuple	curTuple;		/* copy of most recent tuple from subplan */
	/* these are used when hashing the subselect's output: */
	ProjectionInfo *projLeft;	/* for projecting lefthand exprs */
	ProjectionInfo *projRight;	/* for projecting subselect output */
	TupleHashTable hashtable;	/* hash table for no-nulls subselect rows */
	TupleHashTable hashnulls;	/* hash table for rows with null(s) */
	bool		havehashrows;	/* TRUE if hashtable is not empty */
	bool		havenullrows;	/* TRUE if hashnulls is not empty */
	MemoryContext tablecxt;		/* memory context containing tables */
	ExprContext *innerecontext; /* working context for comparisons */
	AttrNumber *keyColIdx;		/* control data for hash tables */
	FmgrInfo   *eqfunctions;	/* comparison functions for hash tables */
	FmgrInfo   *hashfunctions;	/* lookup data for hash functions */
    struct StringInfoData  *cdbextratextbuf;    /* to pass text to cdbexplain */
} SubPlanState;

/* ----------------
 *		FieldSelectState node
 * ----------------
 */
typedef struct FieldSelectState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input expression */
	TupleDesc	argdesc;		/* tupdesc for most recent input */
} FieldSelectState;

/* ----------------
 *		FieldStoreState node
 * ----------------
 */
typedef struct FieldStoreState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input tuple value */
	List	   *newvals;		/* new value(s) for field(s) */
	TupleDesc	argdesc;		/* tupdesc for most recent input */
} FieldStoreState;

/* ----------------
 *		ConvertRowtypeExprState node
 * ----------------
 */
typedef struct ConvertRowtypeExprState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input tuple value */
	TupleDesc	indesc;			/* tupdesc for source rowtype */
	TupleDesc	outdesc;		/* tupdesc for result rowtype */
	AttrNumber *attrMap;		/* indexes of input fields, or 0 for null */
	Datum	   *invalues;		/* workspace for deconstructing source */
	bool	   *inisnull;
	Datum	   *outvalues;		/* workspace for constructing result */
	bool	   *outisnull;
} ConvertRowtypeExprState;

/* ----------------
 *		CaseExprState node
 * ----------------
 */
typedef struct CaseExprState
{
	ExprState	xprstate;
	ExprState  *arg;			/* implicit equality comparison argument */
	List	   *args;			/* the arguments (list of WHEN clauses) */
	ExprState  *defresult;		/* the default result (ELSE clause) */
} CaseExprState;

/* ----------------
 *		CaseWhenState node
 * ----------------
 */
typedef struct CaseWhenState
{
	ExprState	xprstate;
	ExprState  *expr;			/* condition expression */
	ExprState  *result;			/* substitution result */
} CaseWhenState;

/* ----------------
 *		ArrayExprState node
 *
 * Note: ARRAY[] expressions always produce varlena arrays, never fixed-length
 * arrays.
 * ----------------
 */
typedef struct ArrayExprState
{
	ExprState	xprstate;
	List	   *elements;		/* states for child nodes */
	int16		elemlength;		/* typlen of the array element type */
	bool		elembyval;		/* is the element type pass-by-value? */
	char		elemalign;		/* typalign of the element type */
} ArrayExprState;

/* ----------------
 *		RowExprState node
 * ----------------
 */
typedef struct RowExprState
{
	ExprState	xprstate;
	List	   *args;			/* the arguments */
	TupleDesc	tupdesc;		/* descriptor for result tuples */
} RowExprState;

/* ----------------
 *		RowCompareExprState node
 * ----------------
 */
typedef struct RowCompareExprState
{
	ExprState	xprstate;
	List	   *largs;			/* the left-hand input arguments */
	List	   *rargs;			/* the right-hand input arguments */
	FmgrInfo   *funcs;			/* array of comparison function info */
} RowCompareExprState;

/* ----------------
 *		CoalesceExprState node
 * ----------------
 */
typedef struct CoalesceExprState
{
	ExprState	xprstate;
	List	   *args;			/* the arguments */
} CoalesceExprState;

/* ----------------
 *		MinMaxExprState node
 * ----------------
 */
typedef struct MinMaxExprState
{
	ExprState	xprstate;
	List	   *args;			/* the arguments */
	FmgrInfo	cfunc;			/* lookup info for comparison func */
} MinMaxExprState;

/* ----------------
 *		NullTestState node
 * ----------------
 */
typedef struct NullTestState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input expression */
	bool		argisrow;		/* T if input is of a composite type */
	/* used only if argisrow: */
	TupleDesc	argdesc;		/* tupdesc for most recent input */
} NullTestState;

/* ----------------
 *		CoerceToDomainState node
 * ----------------
 */
typedef struct CoerceToDomainState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input expression */
	/* Cached list of constraints that need to be checked */
	List	   *constraints;	/* list of DomainConstraintState nodes */
} CoerceToDomainState;

/*
 * DomainConstraintState - one item to check during CoerceToDomain
 *
 * Note: this is just a Node, and not an ExprState, because it has no
 * corresponding Expr to link to.  Nonetheless it is part of an ExprState
 * tree, so we give it a name following the xxxState convention.
 */
typedef enum DomainConstraintType
{
	DOM_CONSTRAINT_NOTNULL,
	DOM_CONSTRAINT_CHECK
} DomainConstraintType;

typedef struct DomainConstraintState
{
	NodeTag		type;
	DomainConstraintType constrainttype;		/* constraint type */
	char	   *name;			/* name of constraint (for error msgs) */
	ExprState  *check_expr;		/* for CHECK, a boolean expression */
} DomainConstraintState;


/* ----------------------------------------------------------------
 *				 Executor State Trees
 *
 * An executing query has a PlanState tree paralleling the Plan tree
 * that describes the plan.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		PlanState node
 *
 * We never actually instantiate any PlanState nodes; this is just the common
 * abstract superclass for all PlanState-type nodes.
 * ----------------
 */

typedef struct PlanState
{
	NodeTag		type;

	Plan	   *plan;			/* associated Plan node */

	EState	   *state;			/* at execution time, state's of individual
								 * nodes point to one EState for the whole
								 * top-level plan */

	/*
	 * Common structural data for all Plan types.  These links to subsidiary
	 * state trees parallel links in the associated plan tree (except for the
	 * subPlan list, which does not exist in the plan tree).
	 */
	List	   *targetlist;		/* target list to be computed at this node */
	List	   *qual;			/* implicitly-ANDed qual conditions */
	struct PlanState *lefttree; /* input plan tree(s) */
	struct PlanState *righttree;
	List	   *initPlan;		/* Init SubPlanState nodes (un-correlated expr
								 * subselects) */
	List	   *subPlan;		/* SubPlanState nodes in my expressions */

	/*
	 * State for management of parameter-change-driven rescanning
	 */
	Bitmapset  *chgParam;		/* set of IDs of changed Params */

	/*
	 * Other run-time state needed by most if not all node types.
	 */
	TupleTableSlot *ps_OuterTupleSlot;	/* slot for current "outer" tuple */
	TupleTableSlot *ps_ResultTupleSlot; /* slot for my result tuples */
	ExprContext *ps_ExprContext;	/* node's expression-evaluation context */
	ProjectionInfo *ps_ProjInfo;	/* info for doing tuple projection */

    /* 
     * EXPLAIN ANALYZE statistics collection 
     */
    struct Instrumentation *instrument;     /* runtime stats for this node */
    struct StringInfoData  *cdbexplainbuf;  /* EXPLAIN ANALYZE report buf */
    void      (*cdbexplainfun)(struct PlanState *planstate, struct StringInfoData *buf);
                                            /* callback before ExecutorEnd */

} PlanState;

/* ----------------
 *	these are are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlanState(node)		(((PlanState *)(node))->righttree)
#define outerPlanState(node)		(((PlanState *)(node))->lefttree)


/* ----------------
 *	 ResultState information
 * ----------------
 */
typedef struct ResultState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *resconstantqual;
	bool		rs_done;		/* are we done? */
	bool		rs_checkqual;	/* do we need to check the qual? */
	bool		rs_TupFromTlist;/* state flag for processing set-valued
								 * functions in targetlist */
} ResultState;

/* ----------------
 *	 AppendState information
 *
 *		nplans			how many plans are in the list
 *		whichplan		which plan is being executed (0 .. n-1)
 *		firstplan		first plan to execute (usually 0)
 *		lastplan		last plan to execute (usually n-1)
 * ----------------
 */
typedef struct AppendState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **appendplans;	/* array of PlanStates for my inputs */
	int			as_nplans;
	int			as_whichplan;
	int			as_firstplan;
	int			as_lastplan;
} AppendState;

/* ----------------
 *	 BitmapAndState information
 * ----------------
 */
typedef struct BitmapAndState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **bitmapplans;	/* array of PlanStates for my inputs */
	int			nplans;			/* number of input plans */
	Node	   *bitmap;	/* output stream bitmap */
} BitmapAndState;

/* ----------------
 *	 BitmapOrState information
 * ----------------
 */
typedef struct BitmapOrState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **bitmapplans;	/* array of PlanStates for my inputs */
	int			nplans;			/* number of input plans */
	Node 	   *bitmap;			/* output bitmap */
} BitmapOrState;

/* ----------------------------------------------------------------
 *				 Scan State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 ScanState information
 *
 *		ScanState extends PlanState for node types that represent
 *		scans of an underlying relation.  It can also be used for nodes
 *		that scan the output of an underlying plan node --- in that case,
 *		only ScanTupleSlot is actually useful, and it refers to the tuple
 *		retrieved from the subplan.
 *
 *		currentRelation    relation being scanned (NULL if none)
 *		currentScanDesc    current scan descriptor for scan (NULL if none)
 *		ScanTupleSlot	   pointer to slot in tuple table holding scan tuple
 * ----------------
 */
typedef struct ScanState
{
	PlanState	ps;				/* its first field is NodeTag */
	Relation	ss_currentRelation;
	HeapScanDesc ss_currentScanDesc;
	TupleTableSlot *ss_ScanTupleSlot;
	/* CKTAN */
	struct {
		HeapTupleData item[512];
		int bot, top;
		HeapTuple last;
		int seen_EOS;
	} ss_heapTupleData;
	
} ScanState;

/*
 * SeqScan uses a bare ScanState as its state node, since it needs
 * no additional fields.
 */
typedef ScanState SeqScanState;

/*
 * These structs store information about index quals that don't have simple
 * constant right-hand sides.  See comments for ExecIndexBuildScanKeys()
 * for discussion.
 */
typedef struct
{
	ScanKey		scan_key;		/* scankey to put value into */
	ExprState  *key_expr;		/* expr to evaluate to get value */
} IndexRuntimeKeyInfo;

typedef struct
{
	ScanKey		scan_key;		/* scankey to put value into */
	ExprState  *array_expr;		/* expr to evaluate to get array value */
	int			next_elem;		/* next array element to use */
	int			num_elems;		/* number of elems in current array value */
	Datum	   *elem_values;	/* array of num_elems Datums */
	bool	   *elem_nulls;		/* array of num_elems is-null flags */
} IndexArrayKeyInfo;

/* ----------------
 *	 IndexScanState information
 *
 *		indexqualorig	   execution state for indexqualorig expressions
 *		ScanKeys		   Skey structures to scan index rel
 *		NumScanKeys		   number of Skey structs
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys structs
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 * ----------------
 */
typedef struct IndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *indexqualorig;
	ScanKey		iss_ScanKeys;
	int			iss_NumScanKeys;
	IndexRuntimeKeyInfo *iss_RuntimeKeys;
	int			iss_NumRuntimeKeys;
	bool		iss_RuntimeKeysReady;
	ExprContext *iss_RuntimeContext;
	Relation	iss_RelationDesc;
	IndexScanDesc iss_ScanDesc;
} IndexScanState;

/* ----------------
 *	 BitmapIndexScanState information
 *
 *		result			   bitmap to return output into, or NULL
 *		ScanKeys		   Skey structures to scan index rel
 *		NumScanKeys		   number of Skey structs
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys structs
 *		ArrayKeys		   info about Skeys that come from ScalarArrayOpExprs
 *		NumArrayKeys	   number of ArrayKeys structs
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 * ----------------
 */
typedef struct BitmapIndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	Node 	   *bitmap;			/* output bitmap */
	ScanKey		biss_ScanKeys;
	int			biss_NumScanKeys;
	IndexRuntimeKeyInfo *biss_RuntimeKeys;
	int			biss_NumRuntimeKeys;
	IndexArrayKeyInfo *biss_ArrayKeys;
	int			biss_NumArrayKeys;
	bool		biss_RuntimeKeysReady;
	ExprContext *biss_RuntimeContext;
	Relation	biss_RelationDesc;
	IndexScanDesc biss_ScanDesc;
} BitmapIndexScanState;

/* ----------------
 *	 BitmapHeapScanState information
 *
 *		bitmapqualorig	   execution state for bitmapqualorig expressions
 *		tbm				   bitmap obtained from child index scan(s)
 *		tbmres			   current-page data
 * ----------------
 */
typedef struct BitmapHeapScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *bitmapqualorig;
	Node  *tbm;
	TBMIterateResult *tbmres;
} BitmapHeapScanState;

/* ----------------
 *	 TidScanState information
 *
 *		NumTids		   number of tids in this scan
 *		TidPtr		   index of currently fetched tid
 *		TidList		   evaluated item pointers (array of size NumTids)
 * ----------------
 */
typedef struct TidScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *tss_tidquals;	/* list of ExprState nodes */
	int			tss_NumTids;
	int			tss_TidPtr;
	int			tss_MarkTidPtr;
	ItemPointerData *tss_TidList;
	HeapTupleData tss_htup;
} TidScanState;

/* ----------------
 *	 SubqueryScanState information
 *
 *		SubqueryScanState is used for scanning a sub-query in the range table.
 *		The sub-query will have its own EState, which we save here.
 *		ScanTupleSlot references the current output tuple of the sub-query.
 *
 *		SubEState		   exec state for sub-query
 * ----------------
 */
typedef struct SubqueryScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	PlanState  *subplan;
	EState	   *sss_SubEState;
    bool        cdb_want_ctid;  /* true => ctid is referenced in targetlist */
    ItemPointerData cdb_fake_ctid;
    Oid         cdb_fake_tableoid;
} SubqueryScanState;

/* ----------------
 *	 FunctionScanState information
 *
 *		Function nodes are used to scan the results of a
 *		function appearing in FROM (typically a function returning set).
 *
 *		tupdesc				expected return tuple description
 *		tuplestorestate		private state of tuplestore.c
 *		funcexpr			state for function expression being evaluated
 * ----------------
 */
typedef struct FunctionScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	TupleDesc	tupdesc;
	struct Tuplestorestate *tuplestorestate;
	ExprState  *funcexpr;
    bool        cdb_want_ctid;  /* true => ctid is referenced in targetlist */
    ItemPointerData cdb_fake_ctid;
    ItemPointerData cdb_mark_ctid;
    Oid         cdb_fake_tableoid;
} FunctionScanState;

/* ----------------
 *	 ValuesScanState information
 *
 *		ValuesScan nodes are used to scan the results of a VALUES list
 *
 *		rowcontext			per-expression-list context
 *		exprlists			array of expression lists being evaluated
 *		array_len			size of array
 *		curr_idx			current array index (0-based)
 *		marked_idx			marked position (for mark/restore)
 *
 *	Note: ss.ps.ps_ExprContext is used to evaluate any qual or projection
 *	expressions attached to the node.  We create a second ExprContext,
 *	rowcontext, in which to build the executor expression state for each
 *	Values sublist.  Resetting this context lets us get rid of expression
 *	state for each row, avoiding major memory leakage over a long values list.
 * ----------------
 */
typedef struct ValuesScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprContext *rowcontext;
	List	  **exprlists;
	int			array_len;
	int			curr_idx;
	int			marked_idx;
    bool        cdb_want_ctid;  /* true => ctid is referenced in targetlist */
    Oid         cdb_fake_tableoid;
} ValuesScanState;

/* ----------------
 *	 ExternalScanState information
 *
 *		ExternalScan nodes are used to scan external tables
 *
 *		ess_ScanDesc		the state of the file data scan
 * ----------------
 */
typedef struct ExternalScanState
{
	ScanState	ss;
	FileScanDesc	ess_ScanDesc;
} ExternalScanState;

/* ----------------------------------------------------------------
 *				 Join State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 JoinState information
 *
 *		Superclass for state nodes of join plans.
 * ----------------
 */
typedef struct JoinState
{
	PlanState	ps;
	JoinType	jointype;
	List	   *joinqual;		/* JOIN quals (in addition to ps.qual) */
} JoinState;

/* Structure for partial materialize of outer relation in Nested loop and hash joins */
typedef struct executorTupleQueue
{
	int			maxsize;
	int			size;
	int			pos;
	TupleTableSlot *slot;

	HeapTuple	tuples[1];
} executorTupleQueue;

/* ----------------
 *	 NestLoopState information
 *
 *		NeedNewOuter	   true if need new outer tuple on next call
 *		MatchedOuter	   true if found a join match for current outer tuple
 *		NullInnerTupleSlot prepared null tuple for left outer joins
 * ----------------
 */
typedef struct NestLoopState
{
	JoinState	js;				/* its first field is NodeTag */
	bool		nl_NeedNewOuter;
	bool		nl_MatchedOuter;
    bool        nl_Untouched;           /*CDB*/
    bool        nl_QuitIfEmptyInner;    /*CDB*/

	executorTupleQueue *outer_q;

	bool		shared_outer;
	bool		prefetch_inner;

	TupleTableSlot *nl_NullInnerTupleSlot;
} NestLoopState;

/* ----------------
 *	 MergeJoinState information
 *
 *		NumClauses		   number of mergejoinable join clauses
 *		Clauses			   info for each mergejoinable clause
 *		JoinState		   current "state" of join.  see execdefs.h
 *		FillOuter		   true if should emit unjoined outer tuples anyway
 *		FillInner		   true if should emit unjoined inner tuples anyway
 *		MatchedOuter	   true if found a join match for current outer tuple
 *		MatchedInner	   true if found a join match for current inner tuple
 *		OuterTupleSlot	   slot in tuple table for cur outer tuple
 *		InnerTupleSlot	   slot in tuple table for cur inner tuple
 *		MarkedTupleSlot    slot in tuple table for marked tuple
 *		NullOuterTupleSlot prepared null tuple for right outer joins
 *		NullInnerTupleSlot prepared null tuple for left outer joins
 *		OuterEContext	   workspace for computing outer tuple's join values
 *		InnerEContext	   workspace for computing inner tuple's join values
 * ----------------
 */
/* private in nodeMergejoin.c: */
typedef struct MergeJoinClauseData *MergeJoinClause;

typedef struct MergeJoinState
{
	JoinState	js;				/* its first field is NodeTag */
	int			mj_NumClauses;
	MergeJoinClause mj_Clauses; /* array of length mj_NumClauses */
	int			mj_JoinState;
	bool		mj_FillOuter;
	bool		mj_FillInner;
	bool		mj_MatchedOuter;
	bool		mj_MatchedInner;
	TupleTableSlot *mj_OuterTupleSlot;
	TupleTableSlot *mj_InnerTupleSlot;
	TupleTableSlot *mj_MarkedTupleSlot;
	TupleTableSlot *mj_NullOuterTupleSlot;
	TupleTableSlot *mj_NullInnerTupleSlot;
	ExprContext *mj_OuterEContext;
	ExprContext *mj_InnerEContext;
} MergeJoinState;

/* ----------------
 *	 HashJoinState information
 *
 *		hj_HashTable			hash table for the hashjoin
 *								(NULL if table not built yet)
 *		hj_CurHashValue			hash value for current outer tuple
 *		hj_CurBucketNo			bucket# for current outer tuple
 *		hj_CurTuple				last inner tuple matched to current outer
 *								tuple, or NULL if starting search
 *								(CurHashValue, CurBucketNo and CurTuple are
 *								 undefined if OuterTupleSlot is empty!)
 *		hj_OuterHashKeys		the outer hash keys in the hashjoin condition
 *		hj_InnerHashKeys		the inner hash keys in the hashjoin condition
 *		hj_HashOperators		the join operators in the hashjoin condition
 *		hj_OuterTupleSlot		tuple slot for outer tuples
 *		hj_HashTupleSlot		tuple slot for hashed tuples
 *		hj_NullInnerTupleSlot	prepared null tuple for left outer joins
 *		hj_FirstOuterTupleSlot	first tuple retrieved from outer plan
 *		hj_NeedNewOuter			true if need new outer tuple on next call
 *		hj_MatchedOuter			true if found a join match for current outer
 *		hj_OuterNotEmpty		true if outer relation known not empty
 * ----------------
 */

/* these structs are defined in executor/hashjoin.h: */
typedef struct HashJoinTupleData *HashJoinTuple;
typedef struct HashJoinTableData *HashJoinTable;

typedef struct HashJoinState
{
	JoinState	js;				/* its first field is NodeTag */
	List	   *hashclauses;	/* list of ExprState nodes */
	HashJoinTable hj_HashTable;
	uint32		hj_CurHashValue;
	int			hj_CurBucketNo;
	HashJoinTuple hj_CurTuple;
	List	   *hj_OuterHashKeys;		/* list of ExprState nodes */
	List	   *hj_InnerHashKeys;		/* list of ExprState nodes */
	List	   *hj_HashOperators;		/* list of operator OIDs */
	TupleTableSlot *hj_OuterTupleSlot;
	TupleTableSlot *hj_HashTupleSlot;
	TupleTableSlot *hj_NullInnerTupleSlot;
	TupleTableSlot *hj_FirstOuterTupleSlot;
	bool		hj_NeedNewOuter;
	bool		hj_MatchedOuter;
	bool		hj_OuterNotEmpty;

	executorTupleQueue *outer_q;
} HashJoinState;


/* ----------------
 *	 AdpativeNestLoopState information
 *
 *   Record state for two possible subplans.
 *   along with a tuple queue for shared results from the outer plan of
 *   each
 * ----------------
 */
typedef struct AdaptiveNestLoopState
{
	PlanState			ps;
	NestLoopState		*main;
	HashJoinState		*alternate;

	executorTupleQueue	*outer_q;

	int					nl_strategy_threshold;
	int					nl_strategy;
} AdaptiveNestLoopState;

/* ----------------
 *	 MaterialState information
 *
 *		materialize nodes are used to materialize the results
 *		of a subplan into a temporary file.
 *
 *		ss.ss_ScanTupleSlot refers to output of underlying plan.
 * ----------------
 */
typedef struct MaterialState
{
	ScanState	ss;				/* its first field is NodeTag */
	bool		randomAccess;	/* need random access to subplan output? */
	bool		eof_underlying; /* reached end of underlying plan? */

	void	   *ts_state;	    /* private state of tuplestore.c */
	void       *ts_pos;
	void 	   *ts_markpos;
	void  		*share_lk_ctxt;
} MaterialState;

/* ----------------
 *	 ShareInputScanState information
 *
 *	State of each scanner of the ShareInput node
 * ----------------
 */
typedef struct ShareInputScanState
{
	ScanState 	ss;
	/* 
	 * Depends on share_type, we should have a tuplestore_state, tuplestore_pos
	 * or tuplesort_state, tuplesort_pos
	 */
	void 		*ts_state;
	void 		*ts_pos;
	void 		*ts_markpos; 

	void 	    *share_lk_ctxt;
} ShareInputScanState;

/* XXX Should move into buf file */
extern void *shareinput_reader_waitready(int share_id);
extern void *shareinput_writer_notifyready(int share_id, int nsharer_xslice_notify_ready);
extern void shareinput_reader_notifydone(void *, int share_id);
extern void shareinput_writer_waitdone(void *, int share_id, int nsharer_xslice_wait_done);
extern void shareinput_create_bufname_prefix(char* p, int size, int share_id);

/* ----------------
 *	 SortState information
 * ----------------
 */
typedef struct SortState
{
	ScanState	ss;				/* its first field is NodeTag */
	bool		randomAccess;	/* need random access to sort output? */
	bool		sort_Done;		/* sort completed yet? */
	void	   *tuplesortstate; /* private state of tuplesort.c */
	/* CDB */ /* limit state */		
	ExprState  *limitOffset;	/* OFFSET parameter, or NULL if none */
	ExprState  *limitCount;		/* COUNT parameter, or NULL if none */
	bool		noduplicates;   /* true if discard duplicate rows */

	void 		*share_lk_ctxt;
} SortState;

/* ---------------------
 *	GroupState information
 * -------------------------
 */
typedef struct GroupState
{
	ScanState	ss;				/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	bool		grp_done;		/* indicates completion of Group scan */

	HeapTuple	grp_firstTuple; /* copy of first tuple of current group */
	bool        is_passthru;     /* indicate if grp_firstTuple is a pass-thru tuple. */

	/*
	 * This Group node may require to output each group tuple multiple times
	 * since it may represent a grouping set which appears multiple times
	 * in a grouping set. We use prev_groupslot to indicate if a previous output
	 * tuple needs to be output again. How many times this tuple
	 * needs to be outputed is defined in num_repeats.
	 */
	TupleTableSlot *prev_groupslot;
	int             num_repeats;

	/*
	 * The following are used for modifying an input tuple based
	 * on the rollup level of this node.
	 */
	int num_attrs;
	Datum *replValues;
	bool *replIsnull;
	bool *doReplace;
} GroupState;

/* ---------------------
 *	AggState information
 *
 *	ss.ss_ScanTupleSlot refers to output of underlying plan.
 *
 *	Note: ss.ps.ps_ExprContext contains ecxt_aggvalues and
 *	ecxt_aggnulls arrays, which hold the computed agg values for the current
 *	input group during evaluation of an Agg node's output tuple(s).  We
 *	create a second ExprContext, tmpcontext, in which to evaluate input
 *	expressions and run the aggregate transition functions.
 * -------------------------
 */
/* these structs are private in nodeAgg.c: */
typedef struct AggStatePerAggData *AggStatePerAgg;
typedef struct AggStatePerGroupData *AggStatePerGroup;

typedef struct AggState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *aggs;			/* all Aggref nodes in targetlist & quals */
	int			numaggs;		/* length of list (could be zero!) */
	FmgrInfo   *eqfunctions;	/* per-grouping-field equality fns */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	AggStatePerAgg peragg;		/* per-Aggref information */
	MemoryContext aggcontext;	/* memory context for long-lived data */
	ExprContext *tmpcontext;	/* econtext for input expressions */
	bool		agg_done;		/* indicates completion of Agg scan */
	bool        has_partial_agg;/* indicate if a partial aggregate result
								 * has been calculated in the previous call.
								  */

	/* these fields are used in AGG_PLAIN and AGG_SORTED modes: */
	AggStatePerGroup pergroup;	/* per-Aggref-per-group working state */
	HeapTuple	grp_firstTuple; /* copy of first tuple of current group */
	/* these fields are used in AGG_HASHED mode: */
	TupleHashTable hashtable;	/* hash table with one entry per group */
	TupleTableSlot *hashslot;	/* slot for loading hash table */
	List	   *hash_needed;	/* list of columns needed in hash table */
	bool		table_filled;	/* hash table filled yet? */
	TupleHashIterator hashiter; /* for iterating through hash table */
	
	/* MPP */
	MemoryContext transcontext;	/* memory context for less long-lived data */
	struct HashAggTable *hhashtable;

	/* ROLLUP */
	AggStatePerGroup perpassthru; /* per-Aggref-per-pass-through-tuple working state */

	/*
	 * This Agg node may require to output each aggregate tuple multiple times
	 * since it may represent a grouping set which appears multiple times
	 * in a grouping set. We use prev_aggslot to indicate if a previous output
	 * tuple needs to be output again. How many times this tuple
	 * needs to be outputed is defined in num_repeats.
	 */
	TupleTableSlot *prev_aggslot;
	int             num_repeats;

	/*
	 * The following are used to define how to modify input tuples to
	 * satisfy the rollup level of this Agg node.
	 */
	int num_attrs;                /* number of grouping attributes for the Agg node */
	Datum *replValues;
	bool *replIsnull;
	bool *doReplace;
} AggState;


/* ---------------------
 *	WindowState information
 * -------------------------
 */
typedef struct WindowStatePerLevelData *WindowStatePerLevel;
typedef struct WindowStatePerFunctionData *WindowStatePerFunction;
typedef struct WindowInputBufferData *WindowInputBuffer;

typedef struct WindowState
{
	PlanState	ps;				/* its first field is NodeTag */
	List	   *wrxstates;		/* all WindowRefExprState nodes in targetlist */	
	FmgrInfo   *eqfunctions;	/* equality fns for partition key */
	TupleTableSlot *priorslot;	/* place for prior tuple */
	TupleTableSlot *curslot;	/* current tuple */
	TupleTableSlot *spare;	/* current tuple */
	TupleTableSlot *saveslot; /* convenient place holder */

	/* meta data about the current slot */
	bool cur_slot_is_new; /* is this a slot from a buffer or outer plan */
	bool cur_slot_part_break; /* slot breaks the partition key */
	int cur_slot_key_break; /* break level of the key in the slot */
	
	/* Array of working states per distinct window function */
	int			numfuncs;
	WindowStatePerFunction  func_state;
	
	/* Per row state */
	int64		row_index;
	int			numlevels;
	WindowStatePerLevel level_state;

	/* memory context for transition value processing */
	/* XXX: we should probably have one context per level, so that we can
	 * reset it when there's a key change at that level
	 */
	MemoryContext transcontext;

	/* framed window functions need access to their frames */
	WindowStatePerFunction cur_funcstate;

	/* input buffer */
	WindowInputBuffer input_buffer;

	/* Indicate if any function need a peer count. */
	bool need_peercount;
} WindowState;

/* ----------------
 *	 UniqueState information
 *
 *		Unique nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.	Basically
 *		all it does is compare the current tuple from the subplan
 *		with the previously fetched tuple (stored in its result slot).
 *		If the two are identical in all interesting fields, then
 *		we just fetch another tuple from the sort and try again.
 * ----------------
 */
typedef struct UniqueState
{
	PlanState	ps;				/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	MemoryContext tempContext;	/* short-term context for comparisons */
} UniqueState;

/* ----------------
 *	 HashState information
 * ----------------
 */
typedef struct HashState
{
	PlanState	ps;				/* its first field is NodeTag */
	HashJoinTable hashtable;	/* hash table for the hashjoin */
	List	   *hashkeys;		/* list of ExprState nodes */
	/* hashkeys is same as parent's hj_InnerHashKeys */
} HashState;

/* ----------------
 *	 SetOpState information
 *
 *		SetOp nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.	These are
 *		more complex than a simple Unique since we have to count
 *		how many duplicates to return.
 * ----------------
 */
typedef struct SetOpState
{
	PlanState	ps;				/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	bool		subplan_done;	/* has subplan returned EOF? */
	long		numLeft;		/* number of left-input dups of cur group */
	long		numRight;		/* number of right-input dups of cur group */
	long		numOutput;		/* number of dups left to output */
	MemoryContext tempContext;	/* short-term context for comparisons */
} SetOpState;

/* ----------------
 *	 LimitState information
 *
 *		Limit nodes are used to enforce LIMIT/OFFSET clauses.
 *		They just select the desired subrange of their subplan's output.
 *
 * offset is the number of initial tuples to skip (0 does nothing).
 * count is the number of tuples to return after skipping the offset tuples.
 * If no limit count was specified, count is undefined and noCount is true.
 * When lstate == LIMIT_INITIAL, offset/count/noCount haven't been set yet.
 * ----------------
 */
typedef enum
{
	LIMIT_INITIAL,				/* initial state for LIMIT node */
	LIMIT_EMPTY,				/* there are no returnable rows */
	LIMIT_INWINDOW,				/* have returned a row in the window */
	LIMIT_SUBPLANEOF,			/* at EOF of subplan (within window) */
	LIMIT_WINDOWEND,			/* stepped off end of window */
	LIMIT_WINDOWSTART			/* stepped off beginning of window */
} LimitStateCond;

typedef struct LimitState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *limitOffset;	/* OFFSET parameter, or NULL if none */
	ExprState  *limitCount;		/* COUNT parameter, or NULL if none */
	int64		offset;			/* current OFFSET value */
	int64		count;			/* current COUNT, if any */
	bool		noCount;		/* if true, ignore count */
	LimitStateCond lstate;		/* state machine status, as above */
	int64		position;		/* 1-based index of last tuple returned */
	TupleTableSlot *subSlot;	/* tuple last obtained from subplan */
} LimitState;

typedef enum MotionStateType
{
	MOTIONSTATE_NONE,			/* The motion state is not decided, or non active in a slice
								 * (neither send nor recv)
								 */
	MOTIONSTATE_SEND,			/* The motion is sender */
	MOTIONSTATE_RECV,			/* The motion is recver */
} MotionStateType;

/* ----------------
 *	 MotionState information
 * ----------------
 */
typedef struct MotionState
{
	PlanState	ps;				/* its first field is NodeTag */
	MotionStateType mstype;     /* Motion state type */
	bool		stopRequested;  /* set when we want transfer to stop */

	/* For motion send */
	List 		*sliceEntries;      /* for each outgoing slices */
	bool		sentEndOfStream;  	/* set when end-of-stream has successfully been sent */
	List		*hashExpr;		/* state struct used for evaluating the hash expressions */
	CdbHash		*cdbhash;			/* hash api object */

	/* For Motion recv */
	struct CdbHeap		*tupleheap;		/* data structure for match merge in sorted motion node */
	int					routeIdNext;    /* for a sorted motion node, the routeId to get next (same as the routeId last returned ) */
	bool				tupleheapReady;	/* for a sorted motion node, false until we have a tuple from each source segindex */

	/* The following can be used for debugging, usage stats, etc.  */
	int			numTuplesFromChild;	/* Number of tuples received from child */
	int			numTuplesToAMS;		/* Number of tuples from child that were sent to AMS */
	int			numTuplesFromAMS;	/* Number of tuples received from AMS */
	int			numTuplesToParent;	/* Number of tuples either from child or AMS that were sent to parent */
	int			*numTuplesByHashSegIdx; /* Distribution of number of tuples from child by hash seg index */
	
	struct timeval otherTime;  /* time accumulator used in sending motion node to keep track of time
								* spent getting the next tuple (not sending). this could mean time spent
								* in another motion node receiving. */
							
	struct timeval motionTime;  /* time accumulator for time spent in motion node.  For sending motion node
								 * it is just the amount of time actually sending the tuple thru the
								 * interconnect.  For receiving motion node, it is the time spent waiting
								 * and processing of the next incoming tuple.
								 */
		
	Oid                *outputFunArray;     /* output functions for each column (debug only) */
	Slice              *sendSlice;          /* sending slice */
	Slice              *recvSlice;          /* receiving slice */
} MotionState;

#endif   /* EXECNODES_H */
