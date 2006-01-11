/*-------------------------------------------------------------------------
 *
 * serializations.c
 *	  Serialize a PostgreSQL sequential plan/query tree.
 *
 * Portions Copyright (c) 2006, greenplum inc
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <regex.h>

#include "lib/stringinfo.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/execnodes.h"
#include "nodes/relation.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"  /* For get_typlenbyval */
#include "cdb/cdbvars.h"


/*
 * Macros to simplify output of different kinds of fields.	Use these
 * wherever possible to reduce the chance for silly typos.	Note that these
 * hard-wire conventions about the names of the local variables in an Out
 * routine.
 */

/* Write the label for the node type */
#define MODWRITE_NODE_TYPE(nodelabel) \
	appendStringInfoLiteral(str, nodelabel)

/* Write an integer field (anything written as ":fldname %d") */
#define MODWRITE_INT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* Write an unsigned integer field (anything written as ":fldname %u") */
#define MODWRITE_UINT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write an OID field (don't hard-wire assumption that OID is same as uint) */
#define MODWRITE_OID_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write a long-integer field */
#define MODWRITE_LONG_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %ld", node->fldname)

/* Write a char field (ie, one ascii character) */
#define MODWRITE_CHAR_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %c", node->fldname)

/* Write an enumerated-type field as an integer code */
#define MODWRITE_ENUM_FIELD(fldname, enumtype) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", \
					 (int) node->fldname)

/* Write a float field --- caller must give format to define precision */
#define MODWRITE_FLOAT_FIELD(fldname,format) \
	appendStringInfo(str, " :" CppAsString(fldname) " " format, node->fldname)

/* Write a boolean field */
#define MODWRITE_BOOL_FIELD(fldname) \
	(node->fldname ? \
	 appendStringInfoLiteral(str, " :" CppAsString(fldname) " true") : \
	 appendStringInfoLiteral(str, " :" CppAsString(fldname) " false"))

/* Write a character-string (possibly NULL) field */
#define MODWRITE_STRING_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 mod_outToken(str, node->fldname))

/* Write a Node field */
#define MODWRITE_NODE_FIELD(fldname) \
	(appendStringInfoLiteral(str, " :" CppAsString(fldname) " "), \
	 mod_outNode(str, node->fldname))

/* Write a List field: this is a shortcut (you can call
 * MODWRITE_NODE_FIELD for list fields) to avoid an extra call to
 * mod_outNode() */
#define MODWRITE_LIST_FIELD(fldname) \
	((node->fldname == NULL) ? \
	 (appendStringInfo(str, " :" CppAsString(fldname) " "), \
	  appendStringInfoLiteral(str, "<>")) :				\
	 (appendStringInfo(str, " :" CppAsString(fldname) " "), \
	  mod_outList(str, node->fldname)))

/* Write a bitmapset field */
#define MODWRITE_BITMAPSET_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 mod_outBitmapset(str, node->fldname))

/* Write a dummy field -- value not displayable or copyable */
#define MODWRITE_DUMMY_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 mod_outToken(str, NULL))


static void mod_outNode(StringInfo str, void *obj);
char *ModifiedNodeToString(void *obj);
char *serializeThis(Node *node, int *size);


/*
 * mod_outToken
 *	  Convert an ordinary string (eg, an identifier) into a form that
 *	  will be decoded back to a plain token by read.c's functions.
 *
 *	  If a null or empty string is given, it is encoded as "<>".
 */
static void
mod_outToken(StringInfo str, const char *s)
{
	if (s == NULL || *s == '\0')
	{
		appendStringInfoLiteral(str, "<>");
		return;
	}

	/*
	 * Look for characters or patterns that are treated specially by read.c
	 * (either in pg_strtok() or in nodeRead()), and therefore need a
	 * protective backslash.
	 */
	/* These characters only need to be quoted at the start of the string */
	if (*s == '<' ||
		*s == '\"' ||
		isdigit((unsigned char) *s) ||
		((*s == '+' || *s == '-') &&
		 (isdigit((unsigned char) s[1]) || s[1] == '.')))
		appendStringInfoChar(str, '\\');
	while (*s)
	{
		/* These chars must be backslashed anywhere in the string */
		if (*s == ' ' || *s == '\n' || *s == '\t' ||
			*s == '(' || *s == ')' || *s == '{' || *s == '}' ||
			*s == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, *s++);
	}
}

static void
mod_outList(StringInfo str, List *node)
{
	ListCell   *lc;

	appendStringInfoChar(str, '(');

	if (IsA(node, IntList))
		appendStringInfoChar(str, 'i');
	else if (IsA(node, OidList))
		appendStringInfoChar(str, 'o');

	foreach(lc, node)
	{
		/*
		 * For the sake of backward compatibility, we emit a slightly
		 * different whitespace format for lists of nodes vs. other types of
		 * lists. XXX: is this necessary?
		 */
		if (IsA(node, List))
		{
			mod_outNode(str, lfirst(lc));
			if (lnext(lc))
				appendStringInfoChar(str, ' ');
		}
		else if (IsA(node, IntList))
			appendStringInfo(str, " %d", lfirst_int(lc));
		else if (IsA(node, OidList))
			appendStringInfo(str, " %u", lfirst_oid(lc));
		else
			elog(ERROR, "unrecognized list node type: %d",
				 (int) node->type);
	}

	appendStringInfoChar(str, ')');
}

/*
 * mod_outBitmapset -
 *	   converts a bitmap set of integers
 *
 * Note: the output format is "(b int int ...)", similar to an integer List.
 * Currently bitmapsets do not appear in any node type that is stored in
 * rules, so there is no support in readfuncs.c for reading this format.
 */
static void
mod_outBitmapset(StringInfo str, Bitmapset *bms)
{
	Bitmapset  *tmpset;
	int			x;

	appendStringInfoChar(str, '(');
	appendStringInfoChar(str, 'b');
	tmpset = bms_copy(bms);
	while ((x = bms_first_member(tmpset)) >= 0)
		appendStringInfo(str, " %d", x);
	bms_free(tmpset);
	appendStringInfoChar(str, ')');
}

/*
 * Print the value of a Datum given its type.
 */
static void
mod_outDatum(StringInfo str, Datum value, int typlen, bool typbyval)
{
	Size		length,
				i;
	char	   *s;

	length = datumGetSize(value, typbyval, typlen);

	if (typbyval)
	{
		s = (char *) (&value);
		appendStringInfo(str, "%u [ ", (unsigned int) length);
		for (i = 0; i < (Size) sizeof(Datum); i++)
			appendStringInfo(str, "%d ", (int) (s[i]));
		appendStringInfoChar(str, ']');
	}
	else
	{
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
			appendStringInfoLiteral(str, "0 [ ]");
		else
		{
			appendStringInfo(str, "%u [ ", (unsigned int) length);
			for (i = 0; i < length; i++)
				appendStringInfo(str, "%d ", (int) (s[i]));
			appendStringInfoChar(str, ']');
		}
	}
}


/*
 *	Stuff from plannodes.h
 */

/*
 * print the basic stuff of all nodes that inherit from Plan
 */
static void
mod_outPlanInfo(StringInfo str, Plan *node)
{
	MODWRITE_FLOAT_FIELD(startup_cost, "%.2f");
	MODWRITE_FLOAT_FIELD(total_cost, "%.2f");
	MODWRITE_FLOAT_FIELD(plan_rows, "%.0f");
	MODWRITE_INT_FIELD(plan_width);
	MODWRITE_LIST_FIELD(targetlist);
	MODWRITE_LIST_FIELD(qual);
	MODWRITE_BITMAPSET_FIELD(extParam);
	MODWRITE_BITMAPSET_FIELD(allParam);
	MODWRITE_INT_FIELD(nParamExec);
	MODWRITE_NODE_FIELD(flow);
	MODWRITE_ENUM_FIELD(dispatch,DispatchMethod);
	MODWRITE_INT_FIELD(nMotionNodes);
	MODWRITE_NODE_FIELD(sliceTable);

    MODWRITE_NODE_FIELD(lefttree);
    MODWRITE_NODE_FIELD(righttree);
    MODWRITE_NODE_FIELD(initPlan);
}

/*
 * print the basic stuff of all nodes that inherit from Scan
 */
static void
mod_outScanInfo(StringInfo str, Scan *node)
{
	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_UINT_FIELD(scanrelid);
}

/*
 * print the basic stuff of all nodes that inherit from Join
 */
static void
mod_outJoinPlanInfo(StringInfo str, Join *node)
{
	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_ENUM_FIELD(jointype, JoinType);
	MODWRITE_LIST_FIELD(joinqual);
}


static void
mod_outPlan(StringInfo str, Plan *node)
{
	MODWRITE_NODE_TYPE("PLAN");

	mod_outPlanInfo(str, (Plan *) node);
}

static void
mod_outResult(StringInfo str, Result *node)
{
	MODWRITE_NODE_TYPE("RESULT");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_NODE_FIELD(resconstantqual);
}

static void
mod_outAppend(StringInfo str, Append *node)
{
	MODWRITE_NODE_TYPE("APPEND");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_LIST_FIELD(appendplans);
	MODWRITE_BOOL_FIELD(isTarget);
}

static void
mod_outBitmapAnd(StringInfo str, BitmapAnd *node)
{
	MODWRITE_NODE_TYPE("BITMAPAND");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_LIST_FIELD(bitmapplans);
}

static void
mod_outBitmapOr(StringInfo str, BitmapOr *node)
{
	MODWRITE_NODE_TYPE("BITMAPOR");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_LIST_FIELD(bitmapplans);
}

static void
mod_outScan(StringInfo str, Scan *node)
{
	MODWRITE_NODE_TYPE("SCAN");

	mod_outScanInfo(str, (Scan *) node);
}

static void
mod_outSeqScan(StringInfo str, SeqScan *node)
{
	MODWRITE_NODE_TYPE("SEQSCAN");

	mod_outScanInfo(str, (Scan *) node);
}

static void
mod_outIndexScan(StringInfo str, IndexScan *node)
{
	MODWRITE_NODE_TYPE("INDEXSCAN");

	mod_outScanInfo(str, (Scan *) node);

	MODWRITE_OID_FIELD(indexid);
	MODWRITE_LIST_FIELD(indexqual);
	MODWRITE_LIST_FIELD(indexqualorig);
	MODWRITE_LIST_FIELD(indexstrategy);
	MODWRITE_LIST_FIELD(indexsubtype);
	MODWRITE_ENUM_FIELD(indexorderdir, ScanDirection);
}

static void
mod_outBitmapIndexScan(StringInfo str, BitmapIndexScan *node)
{
	MODWRITE_NODE_TYPE("BITMAPINDEXSCAN");

	mod_outScanInfo(str, (Scan *) node);

	MODWRITE_OID_FIELD(indexid);
	MODWRITE_LIST_FIELD(indexqual);
	MODWRITE_LIST_FIELD(indexqualorig);
	MODWRITE_LIST_FIELD(indexstrategy);
	MODWRITE_LIST_FIELD(indexsubtype);
}

static void
mod_outBitmapHeapScan(StringInfo str, BitmapHeapScan *node)
{
	MODWRITE_NODE_TYPE("BITMAPHEAPSCAN");

	mod_outScanInfo(str, (Scan *) node);

	MODWRITE_LIST_FIELD(bitmapqualorig);
}

static void
mod_outTidScan(StringInfo str, TidScan *node)
{
	MODWRITE_NODE_TYPE("TIDSCAN");

	mod_outScanInfo(str, (Scan *) node);

	MODWRITE_LIST_FIELD(tideval);
}

static void
mod_outSubqueryScan(StringInfo str, SubqueryScan *node)
{
	MODWRITE_NODE_TYPE("SUBQUERYSCAN");

	mod_outScanInfo(str, (Scan *) node);

	MODWRITE_NODE_FIELD(subplan);
}

static void
mod_outFunctionScan(StringInfo str, FunctionScan *node)
{
	MODWRITE_NODE_TYPE("FUNCTIONSCAN");

	mod_outScanInfo(str, (Scan *) node);
}

static void
mod_outJoin(StringInfo str, Join *node)
{
	MODWRITE_NODE_TYPE("JOIN");

	mod_outJoinPlanInfo(str, (Join *) node);
}

static void
mod_outNestLoop(StringInfo str, NestLoop *node)
{
	MODWRITE_NODE_TYPE("NESTLOOP");

	mod_outJoinPlanInfo(str, (Join *) node);
}

static void
mod_outMergeJoin(StringInfo str, MergeJoin *node)
{
	MODWRITE_NODE_TYPE("MERGEJOIN");

	mod_outJoinPlanInfo(str, (Join *) node);

	MODWRITE_LIST_FIELD(mergeclauses);
}

static void
mod_outHashJoin(StringInfo str, HashJoin *node)
{
	MODWRITE_NODE_TYPE("HASHJOIN");

	mod_outJoinPlanInfo(str, (Join *) node);

	MODWRITE_LIST_FIELD(hashclauses);
}

static void
mod_outAgg(StringInfo str, Agg *node)
{
	int			i;

	MODWRITE_NODE_TYPE("AGG");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	MODWRITE_INT_FIELD(numCols);

	appendStringInfoLiteral(str, " :grpColIdx");
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, " %d", node->grpColIdx[i]);

	MODWRITE_LONG_FIELD(numGroups);
}

static void
mod_outGroup(StringInfo str, Group *node)
{
	int			i;

	MODWRITE_NODE_TYPE("GROUP");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_INT_FIELD(numCols);

	appendStringInfoLiteral(str, " :grpColIdx");
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, " %d", node->grpColIdx[i]);
}

static void
mod_outMaterial(StringInfo str, Material *node)
{
	MODWRITE_NODE_TYPE("MATERIAL");

    MODWRITE_BOOL_FIELD(cdb_strict);

	mod_outPlanInfo(str, (Plan *) node);
}

static void
mod_outSort(StringInfo str, Sort *node)
{
	int			i;

	MODWRITE_NODE_TYPE("SORT");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_INT_FIELD(numCols);

	appendStringInfoLiteral(str, " :sortColIdx");
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, " %d", node->sortColIdx[i]);

	appendStringInfoLiteral(str, " :sortOperators");
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, " %u", node->sortOperators[i]);
}

static void
mod_outUnique(StringInfo str, Unique *node)
{
	int			i;

	MODWRITE_NODE_TYPE("UNIQUE");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_INT_FIELD(numCols);

	appendStringInfoLiteral(str, " :uniqColIdx");
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, " %d", node->uniqColIdx[i]);
}

static void
mod_outSetOp(StringInfo str, SetOp *node)
{
	int			i;

	MODWRITE_NODE_TYPE("SETOP");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_ENUM_FIELD(cmd, SetOpCmd);
	MODWRITE_INT_FIELD(numCols);

	appendStringInfoLiteral(str, " :dupColIdx");
	for (i = 0; i < node->numCols; i++)
		appendStringInfo(str, " %d", node->dupColIdx[i]);

	MODWRITE_INT_FIELD(flagColIdx);
}

static void
mod_outLimit(StringInfo str, Limit *node)
{
	MODWRITE_NODE_TYPE("LIMIT");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_NODE_FIELD(limitOffset);
	MODWRITE_NODE_FIELD(limitCount);
}

static void
mod_outHash(StringInfo str, Hash *node)
{
	MODWRITE_NODE_TYPE("HASH");

	mod_outPlanInfo(str, (Plan *) node);
}

static void
mod_outMotion(StringInfo str, Motion *node)
{
	int i;

	MODWRITE_NODE_TYPE("MOTION");

	mod_outPlanInfo(str, (Plan *) node);

	MODWRITE_ENUM_FIELD(motionType, MotionType);
	MODWRITE_BOOL_FIELD(sendToSecondary);
	MODWRITE_BOOL_FIELD(sendSorted);
	MODWRITE_INT_FIELD(motionID);	

    MODWRITE_INT_FIELD(numInputSegs);
    if (node->numInputSegs > 0)
    {
        appendStringInfoLiteral(str, " :inputSegIdx");
        if (node->inputSegIdx == NULL)
            appendStringInfoString(str, " <>");
        else
        {
            for (i = 0; i < node->numInputSegs; i++)
                appendStringInfo(str, " %d", node->inputSegIdx[i]);
        }
    }

	MODWRITE_INT_FIELD(numHashSegs);
	appendStringInfoLiteral(str, " :hashSegIdx");
	for (i = 0; i < node->numHashSegs; i++)
		appendStringInfo(str, " %d", node->hashSegIdx[i]);

	MODWRITE_LIST_FIELD(hashExpr);
	MODWRITE_LIST_FIELD(hashDataTypes);
	MODWRITE_BOOL_FIELD(hashToSelfOnly);

	MODWRITE_INT_FIELD(numOutputSegs);
	appendStringInfoLiteral(str, " :outputSegIdx");
	for (i = 0; i < node->numOutputSegs; i++)
		appendStringInfo(str, " %d", node->outputSegIdx[i]);
	MODWRITE_BOOL_FIELD(fixedToSelfOnly);

	MODWRITE_INT_FIELD(numSortCols);
	appendStringInfoLiteral(str, " :sortColIdx");
	for (i = 0; i < node->numSortCols; i++)
		appendStringInfo(str, " %d", node->sortColIdx[i]);

	appendStringInfoLiteral(str, " :sortOperators");
	for (i = 0; i < node->numSortCols; i++)
		appendStringInfo(str, " %u", node->sortOperators[i]);
}

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/

static void
mod_outAlias(StringInfo str, Alias *node)
{
	MODWRITE_NODE_TYPE("ALIAS");

	MODWRITE_STRING_FIELD(aliasname);
	MODWRITE_NODE_FIELD(colnames);
}

static void
mod_outRangeVar(StringInfo str, RangeVar *node)
{
	MODWRITE_NODE_TYPE("RANGEVAR");

	/*
	 * we deliberately ignore catalogname here, since it is presently not
	 * semantically meaningful
	 */
	MODWRITE_STRING_FIELD(schemaname);
	MODWRITE_STRING_FIELD(relname);
	MODWRITE_ENUM_FIELD(inhOpt, InhOption);
	MODWRITE_BOOL_FIELD(istemp);
	MODWRITE_NODE_FIELD(alias);
}

static void
mod_outVar(StringInfo str, Var *node)
{
	MODWRITE_NODE_TYPE("VAR");

	MODWRITE_UINT_FIELD(varno);
	MODWRITE_INT_FIELD(varattno);
	MODWRITE_OID_FIELD(vartype);
	MODWRITE_INT_FIELD(vartypmod);
	MODWRITE_UINT_FIELD(varlevelsup);
	MODWRITE_UINT_FIELD(varnoold);
	MODWRITE_INT_FIELD(varoattno);
}

static void
mod_outConst(StringInfo str, Const *node)
{
	MODWRITE_NODE_TYPE("CONST");

	MODWRITE_OID_FIELD(consttype);
	MODWRITE_INT_FIELD(constlen);
	MODWRITE_BOOL_FIELD(constbyval);
	MODWRITE_BOOL_FIELD(constisnull);

	appendStringInfoLiteral(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfoLiteral(str, "<>");
	else
		mod_outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

static void
mod_outParam(StringInfo str, Param *node)
{
	MODWRITE_NODE_TYPE("PARAM");

	MODWRITE_INT_FIELD(paramkind);
	MODWRITE_INT_FIELD(paramid);
	MODWRITE_STRING_FIELD(paramname);
	MODWRITE_OID_FIELD(paramtype);
}

static void
mod_outAggref(StringInfo str, Aggref *node)
{
	MODWRITE_NODE_TYPE("AGGREF");

	MODWRITE_OID_FIELD(aggfnoid);
	MODWRITE_OID_FIELD(aggtype);
	MODWRITE_NODE_FIELD(target);
	MODWRITE_UINT_FIELD(agglevelsup);
	MODWRITE_BOOL_FIELD(aggstar);
	MODWRITE_BOOL_FIELD(aggdistinct);
}

static void
mod_outArrayRef(StringInfo str, ArrayRef *node)
{
	MODWRITE_NODE_TYPE("ARRAYREF");

	MODWRITE_OID_FIELD(refrestype);
	MODWRITE_OID_FIELD(refarraytype);
	MODWRITE_OID_FIELD(refelemtype);
	MODWRITE_NODE_FIELD(refupperindexpr);
	MODWRITE_NODE_FIELD(reflowerindexpr);
	MODWRITE_NODE_FIELD(refexpr);
	MODWRITE_NODE_FIELD(refassgnexpr);
}

static void
mod_outFuncExpr(StringInfo str, FuncExpr *node)
{
	MODWRITE_NODE_TYPE("FUNCEXPR");

	MODWRITE_OID_FIELD(funcid);
	MODWRITE_OID_FIELD(funcresulttype);
	MODWRITE_BOOL_FIELD(funcretset);
	MODWRITE_ENUM_FIELD(funcformat, CoercionForm);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outOpExpr(StringInfo str, OpExpr *node)
{
	MODWRITE_NODE_TYPE("OPEXPR");

	MODWRITE_OID_FIELD(opno);
	MODWRITE_OID_FIELD(opfuncid);
	MODWRITE_OID_FIELD(opresulttype);
	MODWRITE_BOOL_FIELD(opretset);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outDistinctExpr(StringInfo str, DistinctExpr *node)
{
	MODWRITE_NODE_TYPE("DISTINCTEXPR");

	MODWRITE_OID_FIELD(opno);
	MODWRITE_OID_FIELD(opfuncid);
	MODWRITE_OID_FIELD(opresulttype);
	MODWRITE_BOOL_FIELD(opretset);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outScalarArrayOpExpr(StringInfo str, ScalarArrayOpExpr *node)
{
	MODWRITE_NODE_TYPE("SCALARARRAYOPEXPR");

	MODWRITE_OID_FIELD(opno);
	MODWRITE_OID_FIELD(opfuncid);
	MODWRITE_BOOL_FIELD(useOr);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outBoolExpr(StringInfo str, BoolExpr *node)
{
	char	   *opstr = NULL;

	MODWRITE_NODE_TYPE("BOOLEXPR");

	/* do-it-yourself enum representation */
	switch (node->boolop)
	{
		case AND_EXPR:
			opstr = "and";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
	}
	appendStringInfoLiteral(str, " :boolop ");
	mod_outToken(str, opstr);

	MODWRITE_NODE_FIELD(args);
}

static void
mod_outSubLink(StringInfo str, SubLink *node)
{
	MODWRITE_NODE_TYPE("SUBLINK");

	MODWRITE_ENUM_FIELD(subLinkType, SubLinkType);
	MODWRITE_BOOL_FIELD(useOr);
	MODWRITE_NODE_FIELD(lefthand);
	MODWRITE_NODE_FIELD(operName);
	MODWRITE_NODE_FIELD(operOids);
	MODWRITE_NODE_FIELD(subselect);
}

static void
mod_outSubPlan(StringInfo str, SubPlan *node)
{
	MODWRITE_NODE_TYPE("SUBPLAN");

	MODWRITE_ENUM_FIELD(subLinkType, SubLinkType);
	MODWRITE_BOOL_FIELD(useOr);
	MODWRITE_NODE_FIELD(exprs);
	MODWRITE_NODE_FIELD(paramIds);
	MODWRITE_NODE_FIELD(plan);
	MODWRITE_INT_FIELD(plan_id);
	MODWRITE_NODE_FIELD(rtable);
	MODWRITE_BOOL_FIELD(useHashTable);
	MODWRITE_BOOL_FIELD(unknownEqFalse);
	MODWRITE_NODE_FIELD(setParam);
	MODWRITE_NODE_FIELD(parParam);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outFieldSelect(StringInfo str, FieldSelect *node)
{
	MODWRITE_NODE_TYPE("FIELDSELECT");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_INT_FIELD(fieldnum);
	MODWRITE_OID_FIELD(resulttype);
	MODWRITE_INT_FIELD(resulttypmod);
}

static void
mod_outFieldStore(StringInfo str, FieldStore *node)
{
	MODWRITE_NODE_TYPE("FIELDSTORE");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_NODE_FIELD(newvals);
	MODWRITE_NODE_FIELD(fieldnums);
	MODWRITE_OID_FIELD(resulttype);
}

static void
mod_outRelabelType(StringInfo str, RelabelType *node)
{
	MODWRITE_NODE_TYPE("RELABELTYPE");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_OID_FIELD(resulttype);
	MODWRITE_INT_FIELD(resulttypmod);
	MODWRITE_ENUM_FIELD(relabelformat, CoercionForm);
}

static void
mod_outConvertRowtypeExpr(StringInfo str, ConvertRowtypeExpr *node)
{
	MODWRITE_NODE_TYPE("CONVERTROWTYPEEXPR");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_OID_FIELD(resulttype);
	MODWRITE_ENUM_FIELD(convertformat, CoercionForm);
}

static void
mod_outCaseExpr(StringInfo str, CaseExpr *node)
{
	MODWRITE_NODE_TYPE("CASE");

	MODWRITE_OID_FIELD(casetype);
	MODWRITE_NODE_FIELD(arg);
	MODWRITE_NODE_FIELD(args);
	MODWRITE_NODE_FIELD(defresult);
}

static void
mod_outCaseWhen(StringInfo str, CaseWhen *node)
{
	MODWRITE_NODE_TYPE("WHEN");

	MODWRITE_NODE_FIELD(expr);
	MODWRITE_NODE_FIELD(result);
}

static void
mod_outCaseTestExpr(StringInfo str, CaseTestExpr *node)
{
	MODWRITE_NODE_TYPE("CASETESTEXPR");

	MODWRITE_OID_FIELD(typeId);
	MODWRITE_INT_FIELD(typeMod);
}

static void
mod_outArrayExpr(StringInfo str, ArrayExpr *node)
{
	MODWRITE_NODE_TYPE("ARRAY");

	MODWRITE_OID_FIELD(array_typeid);
	MODWRITE_OID_FIELD(element_typeid);
	MODWRITE_NODE_FIELD(elements);
	MODWRITE_BOOL_FIELD(multidims);
}

static void
mod_outRowExpr(StringInfo str, RowExpr *node)
{
	MODWRITE_NODE_TYPE("ROW");

	MODWRITE_NODE_FIELD(args);
	MODWRITE_OID_FIELD(row_typeid);
	MODWRITE_ENUM_FIELD(row_format, CoercionForm);
}

static void
mod_outCoalesceExpr(StringInfo str, CoalesceExpr *node)
{
	MODWRITE_NODE_TYPE("COALESCE");

	MODWRITE_OID_FIELD(coalescetype);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outMinMaxExpr(StringInfo str, MinMaxExpr *node)
{
	MODWRITE_NODE_TYPE("MINMAX");

	MODWRITE_OID_FIELD(minmaxtype);
	MODWRITE_ENUM_FIELD(op, MinMaxOp);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outNullIfExpr(StringInfo str, NullIfExpr *node)
{
	MODWRITE_NODE_TYPE("NULLIFEXPR");

	MODWRITE_OID_FIELD(opno);
	MODWRITE_OID_FIELD(opfuncid);
	MODWRITE_OID_FIELD(opresulttype);
	MODWRITE_BOOL_FIELD(opretset);
	MODWRITE_NODE_FIELD(args);
}

static void
mod_outNullTest(StringInfo str, NullTest *node)
{
	MODWRITE_NODE_TYPE("NULLTEST");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_ENUM_FIELD(nulltesttype, NullTestType);
}

static void
mod_outBooleanTest(StringInfo str, BooleanTest *node)
{
	MODWRITE_NODE_TYPE("BOOLEANTEST");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_ENUM_FIELD(booltesttype, BoolTestType);
}

static void
mod_outCoerceToDomain(StringInfo str, CoerceToDomain *node)
{
	MODWRITE_NODE_TYPE("COERCETODOMAIN");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_OID_FIELD(resulttype);
	MODWRITE_INT_FIELD(resulttypmod);
	MODWRITE_ENUM_FIELD(coercionformat, CoercionForm);
}

static void
mod_outCoerceToDomainValue(StringInfo str, CoerceToDomainValue *node)
{
	MODWRITE_NODE_TYPE("COERCETODOMAINVALUE");

	MODWRITE_OID_FIELD(typeId);
	MODWRITE_INT_FIELD(typeMod);
}

static void
mod_outSetToDefault(StringInfo str, SetToDefault *node)
{
	MODWRITE_NODE_TYPE("SETTODEFAULT");

	MODWRITE_OID_FIELD(typeId);
	MODWRITE_INT_FIELD(typeMod);
}

static void
mod_outTargetEntry(StringInfo str, TargetEntry *node)
{
	MODWRITE_NODE_TYPE("TARGETENTRY");

	MODWRITE_NODE_FIELD(expr);
	MODWRITE_INT_FIELD(resno);
	MODWRITE_STRING_FIELD(resname);
	MODWRITE_UINT_FIELD(ressortgroupref);
	MODWRITE_OID_FIELD(resorigtbl);
	MODWRITE_INT_FIELD(resorigcol);
	MODWRITE_BOOL_FIELD(resjunk);
}

static void
mod_outRangeTblRef(StringInfo str, RangeTblRef *node)
{
	MODWRITE_NODE_TYPE("RANGETBLREF");

	MODWRITE_INT_FIELD(rtindex);
}

static void
mod_outJoinExpr(StringInfo str, JoinExpr *node)
{
	MODWRITE_NODE_TYPE("JOINEXPR");

	MODWRITE_ENUM_FIELD(jointype, JoinType);
	MODWRITE_BOOL_FIELD(isNatural);
	MODWRITE_NODE_FIELD(larg);
	MODWRITE_NODE_FIELD(rarg);
	MODWRITE_NODE_FIELD(using);
	MODWRITE_NODE_FIELD(quals);
	MODWRITE_NODE_FIELD(alias);
	MODWRITE_INT_FIELD(rtindex);
}

static void
mod_outFromExpr(StringInfo str, FromExpr *node)
{
	MODWRITE_NODE_TYPE("FROMEXPR");

	MODWRITE_NODE_FIELD(fromlist);
	MODWRITE_NODE_FIELD(quals);
}

static void
mod_outFlow(StringInfo str, Flow *node)
{
	int i;
	
	MODWRITE_NODE_TYPE("FLOW");

	MODWRITE_ENUM_FIELD(flotype, FlowType);
	MODWRITE_ENUM_FIELD(base_flotype, FlowType);
	MODWRITE_ENUM_FIELD(req_move, Movement);
	MODWRITE_ENUM_FIELD(req_xform, Movement);
	MODWRITE_INT_FIELD(segindex);
	
	/* This array format as in Group and Sort nodes. */
	MODWRITE_INT_FIELD(numSortCols);
	
	appendStringInfoLiteral(str, " :sortColIdx");
	for ( i = 0; i < node->numSortCols; i++ )
		appendStringInfo(str, " %d", node->sortColIdx[i]);
	
	appendStringInfoLiteral(str, " :sortOperators");
	for ( i = 0; i < node->numSortCols; i++ )
		appendStringInfo(str, " %u", node->sortOperators[i]);
	
	MODWRITE_NODE_FIELD(hashExpr);
}

/*****************************************************************************
 *
 *	Stuff from cdbpathlocus.h.
 *
 *****************************************************************************/

/*
 * mod_outCdbPathLocus
 */
static void
mod_outCdbPathLocus(StringInfo str, CdbPathLocus *node)
{
    MODWRITE_ENUM_FIELD(locustype, CdbLocusType);
    MODWRITE_NODE_FIELD(partkey);
}                               /* mod_outCdbPathLocus */


/*****************************************************************************
 *
 *	Stuff from relation.h.
 *
 *****************************************************************************/

/*
 * print the basic stuff of all nodes that inherit from Path
 *
 * Note we do NOT print the parent, else we'd be in infinite recursion
 */
static void
mod_outPathInfo(StringInfo str, Path *node)
{
	MODWRITE_ENUM_FIELD(pathtype, NodeTag);
	MODWRITE_FLOAT_FIELD(startup_cost, "%.2f");
	MODWRITE_FLOAT_FIELD(total_cost, "%.2f");
    MODWRITE_NODE_FIELD(parent);
    mod_outCdbPathLocus(str, &node->locus);
	MODWRITE_NODE_FIELD(pathkeys);
}

/*
 * print the basic stuff of all nodes that inherit from JoinPath
 */
static void
mod_outJoinPathInfo(StringInfo str, JoinPath *node)
{
	mod_outPathInfo(str, (Path *) node);

	MODWRITE_ENUM_FIELD(jointype, JoinType);
	MODWRITE_NODE_FIELD(outerjoinpath);
	MODWRITE_NODE_FIELD(innerjoinpath);
	MODWRITE_NODE_FIELD(joinrestrictinfo);
}

static void
mod_outPath(StringInfo str, Path *node)
{
	MODWRITE_NODE_TYPE("PATH");

	mod_outPathInfo(str, (Path *) node);
}

static void
mod_outIndexPath(StringInfo str, IndexPath *node)
{
	MODWRITE_NODE_TYPE("INDEXPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(indexinfo);
	MODWRITE_NODE_FIELD(indexclauses);
	MODWRITE_NODE_FIELD(indexquals);
	MODWRITE_BOOL_FIELD(isjoininner);
	MODWRITE_ENUM_FIELD(indexscandir, ScanDirection);
	MODWRITE_FLOAT_FIELD(indextotalcost, "%.2f");
	MODWRITE_FLOAT_FIELD(indexselectivity, "%.4f");
	MODWRITE_FLOAT_FIELD(rows, "%.0f");
}

static void
mod_outBitmapHeapPath(StringInfo str, BitmapHeapPath *node)
{
	MODWRITE_NODE_TYPE("BITMAPHEAPPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(bitmapqual);
	MODWRITE_BOOL_FIELD(isjoininner);
	MODWRITE_FLOAT_FIELD(rows, "%.0f");
}

static void
mod_outBitmapAndPath(StringInfo str, BitmapAndPath *node)
{
	MODWRITE_NODE_TYPE("BITMAPANDPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(bitmapquals);
	MODWRITE_FLOAT_FIELD(bitmapselectivity, "%.4f");
}

static void
mod_outBitmapOrPath(StringInfo str, BitmapOrPath *node)
{
	MODWRITE_NODE_TYPE("BITMAPORPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(bitmapquals);
	MODWRITE_FLOAT_FIELD(bitmapselectivity, "%.4f");
}

static void
mod_outTidPath(StringInfo str, TidPath *node)
{
	MODWRITE_NODE_TYPE("TIDPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(tideval);
}

static void
mod_outAppendPath(StringInfo str, AppendPath *node)
{
	MODWRITE_NODE_TYPE("APPENDPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(subpaths);
}

static void
mod_outResultPath(StringInfo str, ResultPath *node)
{
	MODWRITE_NODE_TYPE("RESULTPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(subpath);
	MODWRITE_NODE_FIELD(constantqual);
}

static void
mod_outMaterialPath(StringInfo str, MaterialPath *node)
{
	MODWRITE_NODE_TYPE("MATERIALPATH");

	mod_outPathInfo(str, (Path *) node);
    MODWRITE_BOOL_FIELD(cdb_strict);

	MODWRITE_NODE_FIELD(subpath);
}

static void
mod_outUniquePath(StringInfo str, UniquePath *node)
{
	MODWRITE_NODE_TYPE("UNIQUEPATH");

	mod_outPathInfo(str, (Path *) node);

	MODWRITE_NODE_FIELD(subpath);
	MODWRITE_ENUM_FIELD(umethod, UniquePathMethod);
	MODWRITE_FLOAT_FIELD(rows, "%.0f");
}

static void
mod_outNestPath(StringInfo str, NestPath *node)
{
	MODWRITE_NODE_TYPE("NESTPATH");

	mod_outJoinPathInfo(str, (JoinPath *) node);
}

static void
mod_outMergePath(StringInfo str, MergePath *node)
{
	MODWRITE_NODE_TYPE("MERGEPATH");

	mod_outJoinPathInfo(str, (JoinPath *) node);

	MODWRITE_NODE_FIELD(path_mergeclauses);
	MODWRITE_NODE_FIELD(outersortkeys);
	MODWRITE_NODE_FIELD(innersortkeys);
}

static void
mod_outHashPath(StringInfo str, HashPath *node)
{
	MODWRITE_NODE_TYPE("HASHPATH");

	mod_outJoinPathInfo(str, (JoinPath *) node);

	MODWRITE_NODE_FIELD(path_hashclauses);
}

static void
mod_outCdbMotionPath(StringInfo str, CdbMotionPath *node)
{
    MODWRITE_NODE_TYPE("MOTIONPATH");

    mod_outPathInfo(str, &node->path);
    MODWRITE_BOOL_FIELD(hashToSelfOnly);

    MODWRITE_NODE_FIELD(subpath);
}

static void
mod_outPlannerInfo(StringInfo str, PlannerInfo *node)
{
	MODWRITE_NODE_TYPE("PLANNERINFO");

	/* NB: this isn't a complete set of fields */
	MODWRITE_NODE_FIELD(parse);
	MODWRITE_NODE_FIELD(join_rel_list);
	MODWRITE_NODE_FIELD(equi_key_list);
	MODWRITE_NODE_FIELD(left_join_clauses);
	MODWRITE_NODE_FIELD(right_join_clauses);
	MODWRITE_NODE_FIELD(full_join_clauses);
	MODWRITE_NODE_FIELD(in_info_list);
	MODWRITE_NODE_FIELD(query_pathkeys);
	MODWRITE_NODE_FIELD(group_pathkeys);
	MODWRITE_NODE_FIELD(sort_pathkeys);
	MODWRITE_FLOAT_FIELD(tuple_fraction, "%.4f");
	MODWRITE_BOOL_FIELD(hasJoinRTEs);
	MODWRITE_BOOL_FIELD(hasOuterJoins);
	MODWRITE_BOOL_FIELD(hasHavingQual);
}

static void
mod_outRelOptInfo(StringInfo str, RelOptInfo *node)
{
	MODWRITE_NODE_TYPE("RELOPTINFO");

	/* NB: this isn't a complete set of fields */
	MODWRITE_ENUM_FIELD(type, NodeTag);
	MODWRITE_ENUM_FIELD(reloptkind, RelOptKind);
	MODWRITE_BITMAPSET_FIELD(relids);
	MODWRITE_FLOAT_FIELD(rows, "%.0f");
	MODWRITE_INT_FIELD(width);
	MODWRITE_NODE_FIELD(reltargetlist);
	MODWRITE_NODE_FIELD(pathlist);
	MODWRITE_NODE_FIELD(cheapest_startup_path);
	MODWRITE_NODE_FIELD(cheapest_total_path);
	MODWRITE_NODE_FIELD(cheapest_unique_path);
	MODWRITE_UINT_FIELD(relid);
	MODWRITE_ENUM_FIELD(rtekind, RTEKind);
	MODWRITE_UINT_FIELD(min_attr);
	MODWRITE_UINT_FIELD(max_attr);
	MODWRITE_NODE_FIELD(indexlist);
	MODWRITE_UINT_FIELD(pages);
	MODWRITE_FLOAT_FIELD(tuples, "%.0f");
	MODWRITE_NODE_FIELD(subplan);
	MODWRITE_NODE_FIELD(baserestrictinfo);
	MODWRITE_BITMAPSET_FIELD(outerjoinset);
	MODWRITE_NODE_FIELD(joininfo);
	MODWRITE_BITMAPSET_FIELD(index_outer_relids);
	MODWRITE_NODE_FIELD(index_inner_paths);
}

static void
mod_outIndexOptInfo(StringInfo str, IndexOptInfo *node)
{
	MODWRITE_NODE_TYPE("INDEXOPTINFO");

	/* NB: this isn't a complete set of fields */
	MODWRITE_OID_FIELD(indexoid);
	/* Do NOT print rel field, else infinite recursion */
	MODWRITE_UINT_FIELD(pages);
	MODWRITE_FLOAT_FIELD(tuples, "%.0f");
	MODWRITE_INT_FIELD(ncolumns);
	MODWRITE_NODE_FIELD(indexprs);
	MODWRITE_NODE_FIELD(indpred);
	MODWRITE_BOOL_FIELD(predOK);
	MODWRITE_BOOL_FIELD(unique);
}

static void
mod_outPathKeyItem(StringInfo str, PathKeyItem *node)
{
	MODWRITE_NODE_TYPE("PATHKEYITEM");

	MODWRITE_NODE_FIELD(key);
	MODWRITE_OID_FIELD(sortop);
}

static void
mod_outRestrictInfo(StringInfo str, RestrictInfo *node)
{
	MODWRITE_NODE_TYPE("RESTRICTINFO");

	/* NB: this isn't a complete set of fields */
	MODWRITE_NODE_FIELD(clause);
	MODWRITE_BOOL_FIELD(is_pushed_down);
	MODWRITE_BOOL_FIELD(outerjoin_delayed);
	MODWRITE_BOOL_FIELD(can_join);
	MODWRITE_BITMAPSET_FIELD(clause_relids);
	MODWRITE_BITMAPSET_FIELD(required_relids);
	MODWRITE_BITMAPSET_FIELD(left_relids);
	MODWRITE_BITMAPSET_FIELD(right_relids);
	MODWRITE_NODE_FIELD(orclause);
	MODWRITE_OID_FIELD(mergejoinoperator);
	MODWRITE_OID_FIELD(left_sortop);
	MODWRITE_OID_FIELD(right_sortop);
	MODWRITE_NODE_FIELD(left_pathkey);
	MODWRITE_NODE_FIELD(right_pathkey);
	MODWRITE_OID_FIELD(hashjoinoperator);
}

static void
mod_outInnerIndexscanInfo(StringInfo str, InnerIndexscanInfo *node)
{
	MODWRITE_NODE_TYPE("INNERINDEXSCANINFO");
	MODWRITE_BITMAPSET_FIELD(other_relids);
	MODWRITE_BOOL_FIELD(isouterjoin);
	MODWRITE_NODE_FIELD(best_innerpath);
}

static void
mod_outInClauseInfo(StringInfo str, InClauseInfo *node)
{
	MODWRITE_NODE_TYPE("INCLAUSEINFO");

	MODWRITE_BITMAPSET_FIELD(lefthand);
	MODWRITE_BITMAPSET_FIELD(righthand);
	MODWRITE_NODE_FIELD(sub_targetlist);
}

/*****************************************************************************
 *
 *	Stuff from parsenodes.h.
 *
 *****************************************************************************/

static void
mod_outCreateStmt(StringInfo str, CreateStmt *node)
{
	MODWRITE_NODE_TYPE("CREATESTMT");

	MODWRITE_NODE_FIELD(relation);
	MODWRITE_NODE_FIELD(tableElts);
	MODWRITE_NODE_FIELD(inhRelations);
	MODWRITE_NODE_FIELD(constraints);
	MODWRITE_ENUM_FIELD(hasoids, ContainsOids);
	MODWRITE_ENUM_FIELD(oncommit, OnCommitAction);
	MODWRITE_STRING_FIELD(tablespacename);
	MODWRITE_NODE_FIELD(distributedBy);
	MODWRITE_OID_FIELD(relOid);
	MODWRITE_OID_FIELD(toastOid);
	MODWRITE_CHAR_FIELD(relKind);
}

static void
mod_outIndexStmt(StringInfo str, IndexStmt *node)
{
	MODWRITE_NODE_TYPE("INDEXSTMT");

	MODWRITE_STRING_FIELD(idxname);
	MODWRITE_NODE_FIELD(relation);
	MODWRITE_STRING_FIELD(accessMethod);
	MODWRITE_STRING_FIELD(tableSpace);
	MODWRITE_NODE_FIELD(indexParams);
	MODWRITE_NODE_FIELD(whereClause);
	MODWRITE_NODE_FIELD(rangetable);
	MODWRITE_BOOL_FIELD(unique);
	MODWRITE_BOOL_FIELD(primary);
	MODWRITE_BOOL_FIELD(isconstraint);
	MODWRITE_OID_FIELD(idxOid);
}

static void
mod_outReindexStmt(StringInfo str, ReindexStmt *node)
{
	MODWRITE_NODE_TYPE("REINDEXSTMT");
	
	MODWRITE_ENUM_FIELD(kind,ObjectType);
	MODWRITE_NODE_FIELD(relation);
	MODWRITE_STRING_FIELD(name);
	MODWRITE_BOOL_FIELD(do_system);
	MODWRITE_BOOL_FIELD(do_user);
	
}


static void
mod_outViewStmt(StringInfo str, ViewStmt *node)
{
	MODWRITE_NODE_TYPE("VIEWSTMT");

	MODWRITE_NODE_FIELD(view);
	MODWRITE_NODE_FIELD(aliases);
	MODWRITE_NODE_FIELD(query);
	MODWRITE_BOOL_FIELD(replace);
	MODWRITE_OID_FIELD(relOid);
}

static void
mod_outRuleStmt(StringInfo str, RuleStmt *node)
{
	MODWRITE_NODE_TYPE("RULESTMT");

	MODWRITE_NODE_FIELD(relation);
	MODWRITE_STRING_FIELD(rulename);	
	MODWRITE_NODE_FIELD(whereClause);
	MODWRITE_ENUM_FIELD(event,CmdType);
	MODWRITE_BOOL_FIELD(instead);
	MODWRITE_NODE_FIELD(actions);	
	MODWRITE_BOOL_FIELD(replace);
}

static void
mod_outDropStmt(StringInfo str, DropStmt *node)
{
	MODWRITE_NODE_TYPE("DROPSTMT");

	MODWRITE_NODE_FIELD(objects);
	MODWRITE_ENUM_FIELD(removeType, ObjectType);
	MODWRITE_ENUM_FIELD(behavior, DropBehavior);

	
}

static void
mod_outTruncateStmt(StringInfo str, TruncateStmt *node)
{
	MODWRITE_NODE_TYPE("TRUNCATESTMT");

	MODWRITE_NODE_FIELD(relations);
	MODWRITE_NODE_FIELD(newOidsList);
	
}

static void
mod_outAlterTableStmt(StringInfo str, AlterTableStmt *node)
{
	MODWRITE_NODE_TYPE("ALTERTABLESTMT");

	MODWRITE_NODE_FIELD(relation);
	MODWRITE_NODE_FIELD(cmds);
	MODWRITE_ENUM_FIELD(relkind, ObjectType);
}

static void
mod_outAlterTableCmd(StringInfo str, AlterTableCmd *node)
{
	MODWRITE_NODE_TYPE("ALTERTABLECMD");

	MODWRITE_ENUM_FIELD(subtype, AlterTableType);
	MODWRITE_STRING_FIELD(name);
	MODWRITE_NODE_FIELD(def);
	MODWRITE_NODE_FIELD(transform);
	MODWRITE_ENUM_FIELD(behavior, DropBehavior);
}


static void
mod_outCreateRoleStmt(StringInfo str, CreateRoleStmt *node)
{
	MODWRITE_NODE_TYPE("CREATEROLESTMT");

	MODWRITE_ENUM_FIELD(stmt_type, RoleStmtType);
	MODWRITE_STRING_FIELD(role);
	MODWRITE_NODE_FIELD(options);

}

static  void
mod_outDropRoleStmt(StringInfo str, DropRoleStmt *node)
{
	MODWRITE_NODE_TYPE("DROPROLESTMT");

	MODWRITE_NODE_FIELD(roles);
}

static  void
mod_outAlterRoleStmt(StringInfo str, AlterRoleStmt *node)
{
	MODWRITE_NODE_TYPE("ALTERROLESTMT");

	MODWRITE_STRING_FIELD(role);
	MODWRITE_NODE_FIELD(options);
	MODWRITE_INT_FIELD(action);
}

static  void
mod_outAlterRoleSetStmt(StringInfo str, AlterRoleSetStmt *node)
{
	MODWRITE_NODE_TYPE("ALTERROLESETSTMT");

	MODWRITE_STRING_FIELD(role);
	MODWRITE_STRING_FIELD(variable);
	MODWRITE_NODE_FIELD(value);
}





static void
mod_outRenameStmt(StringInfo str, RenameStmt *node)
{
	MODWRITE_NODE_TYPE("RENAMESTMT");

	MODWRITE_NODE_FIELD(relation);
	MODWRITE_NODE_FIELD(object);
	MODWRITE_NODE_FIELD(objarg);
	MODWRITE_STRING_FIELD(subname);
	MODWRITE_STRING_FIELD(newname);
	MODWRITE_ENUM_FIELD(renameType,ObjectType);

	
}

static void
mod_outCreateSeqStmt(StringInfo str, CreateSeqStmt *node)
{
	MODWRITE_NODE_TYPE("CREATESEQSTMT");
	MODWRITE_NODE_FIELD(sequence);
	MODWRITE_NODE_FIELD(options);
	MODWRITE_OID_FIELD(relOid);
}

static void
mod_outCreatedbStmt(StringInfo str, CreatedbStmt *node)
{
	MODWRITE_NODE_TYPE("CREATEDBSTMT");
	MODWRITE_STRING_FIELD(dbname);
	MODWRITE_NODE_FIELD(options);
	MODWRITE_OID_FIELD(dbOid);
}

static void
mod_outCreateDomainStmt(StringInfo str, CreateDomainStmt *node)
{
	MODWRITE_NODE_TYPE("CREATEDOMAINSTMT");
	MODWRITE_NODE_FIELD(domainname);
	MODWRITE_NODE_FIELD(typename);
	MODWRITE_NODE_FIELD(constraints);
	MODWRITE_OID_FIELD(domainOid);
}

static void
mod_outAlterDomainStmt(StringInfo str, AlterDomainStmt *node)
{
	MODWRITE_NODE_TYPE("ALTERDOMAINSTMT");
	MODWRITE_CHAR_FIELD(subtype);
	MODWRITE_NODE_FIELD(typename);
	MODWRITE_STRING_FIELD(name);
	MODWRITE_NODE_FIELD(def);
	MODWRITE_ENUM_FIELD(behavior, DropBehavior);
}

static void
mod_outCreateFunctionStmt(StringInfo str, CreateFunctionStmt *node)
{
	MODWRITE_NODE_TYPE("CREATEFUNCSTMT");
	MODWRITE_BOOL_FIELD(replace);
	MODWRITE_NODE_FIELD(funcname);
	MODWRITE_NODE_FIELD(parameters);
	MODWRITE_NODE_FIELD(returnType);
	MODWRITE_NODE_FIELD(options);
	MODWRITE_NODE_FIELD(withClause);
	MODWRITE_OID_FIELD(funcOid);
}

static void
mod_outFunctionParameter(StringInfo str, FunctionParameter *node)
{
	MODWRITE_NODE_TYPE("FUNCTIONPARAMETER");
	MODWRITE_STRING_FIELD(name);
	MODWRITE_NODE_FIELD(argType);
	/* someday add IN/OUT/INOUT indicator here */
}

static void
mod_outRemoveFuncStmt(StringInfo str, RemoveFuncStmt *node)
{
	MODWRITE_NODE_TYPE("REMOVEFUNCSTMT");
	MODWRITE_NODE_FIELD(funcname);
	MODWRITE_NODE_FIELD(args);
	MODWRITE_ENUM_FIELD(behavior, DropBehavior);
}


static void
mod_outTransactionStmt(StringInfo str, TransactionStmt *node)
{
	MODWRITE_NODE_TYPE("TRANSACTIONSTMT");

	MODWRITE_ENUM_FIELD(kind, TransactionStmtKind);
	MODWRITE_NODE_FIELD(options);

}

static void
mod_outNotifyStmt(StringInfo str, NotifyStmt *node)
{
	MODWRITE_NODE_TYPE("NOTIFY");

	MODWRITE_NODE_FIELD(relation);
}

static void
mod_outDeclareCursorStmt(StringInfo str, DeclareCursorStmt *node)
{
	MODWRITE_NODE_TYPE("DECLARECURSOR");

	MODWRITE_STRING_FIELD(portalname);
	MODWRITE_INT_FIELD(options);
	MODWRITE_NODE_FIELD(query);
}

static void
mod_outCopyStmt(StringInfo str, CopyStmt *node)
{
	MODWRITE_NODE_TYPE("COPYSTMT");
	MODWRITE_NODE_FIELD(relation);
	MODWRITE_NODE_FIELD(attlist);
	MODWRITE_BOOL_FIELD(is_from);
	MODWRITE_STRING_FIELD(filename);
	MODWRITE_NODE_FIELD(options);
}

static void
mod_outCdbExecStmt(StringInfo str, CdbExecStmt *node)
{
	MODWRITE_NODE_TYPE("MPPEXECSTMT");
	MODWRITE_NODE_FIELD(ser_query);
	MODWRITE_NODE_FIELD(ser_plan);
	MODWRITE_NODE_FIELD(ser_params);
	MODWRITE_NODE_FIELD(ser_sliceinfo);
}

static void
mod_outGrantStmt(StringInfo str, GrantStmt *node)
{
	MODWRITE_NODE_TYPE("GRANTSTMT");
	MODWRITE_BOOL_FIELD(is_grant);		
	MODWRITE_ENUM_FIELD(objtype,GrantObjectType);	
	MODWRITE_NODE_FIELD(objects);		
	MODWRITE_NODE_FIELD(privileges);		
	MODWRITE_NODE_FIELD(grantees);		
	MODWRITE_BOOL_FIELD(grant_option);	
	MODWRITE_ENUM_FIELD(behavior, DropBehavior);		
}

static void
mod_outPrivGrantee(StringInfo str, PrivGrantee *node)
{
	MODWRITE_NODE_TYPE("PRIVGRANTEE");
	MODWRITE_STRING_FIELD(rolname);	
}

static void
mod_outFuncWithArgs(StringInfo str, FuncWithArgs *node)
{
	MODWRITE_NODE_TYPE("FUNCWITHARGS");
	MODWRITE_NODE_FIELD(funcname);		
	MODWRITE_NODE_FIELD(funcargs);		 	
}

static void
mod_outLockStmt(StringInfo str, LockStmt *node)
{
	MODWRITE_NODE_TYPE("LOCKSTMT");
	MODWRITE_NODE_FIELD(relations);
	MODWRITE_INT_FIELD(mode);
	MODWRITE_BOOL_FIELD(nowait);
}


static void
mod_outSelectStmt(StringInfo str, SelectStmt *node)
{
	MODWRITE_NODE_TYPE("SELECT");

	MODWRITE_NODE_FIELD(distinctClause);
	MODWRITE_NODE_FIELD(into);
	MODWRITE_NODE_FIELD(intoColNames);
	MODWRITE_ENUM_FIELD(intoHasOids, ContainsOids);
	MODWRITE_NODE_FIELD(targetList);
	MODWRITE_NODE_FIELD(fromClause);
	MODWRITE_NODE_FIELD(whereClause);
	MODWRITE_NODE_FIELD(groupClause);
	MODWRITE_NODE_FIELD(havingClause);
	MODWRITE_NODE_FIELD(sortClause);
	MODWRITE_NODE_FIELD(limitOffset);
	MODWRITE_NODE_FIELD(limitCount);
	MODWRITE_NODE_FIELD(lockingClause);
	MODWRITE_ENUM_FIELD(op, SetOperation);
	MODWRITE_BOOL_FIELD(all);
	MODWRITE_NODE_FIELD(larg);
	MODWRITE_NODE_FIELD(rarg);
}

static void
mod_outFuncCall(StringInfo str, FuncCall *node)
{
	MODWRITE_NODE_TYPE("FUNCCALL");

	MODWRITE_NODE_FIELD(funcname);
	MODWRITE_NODE_FIELD(args);
	MODWRITE_BOOL_FIELD(agg_star);
	MODWRITE_BOOL_FIELD(agg_distinct);
}

static void
mod_outDefElem(StringInfo str, DefElem *node)
{
	MODWRITE_NODE_TYPE("DEFELEM");

	MODWRITE_STRING_FIELD(defname);
	MODWRITE_NODE_FIELD(arg);
}

static void
mod_outLockingClause(StringInfo str, LockingClause *node)
{
	MODWRITE_NODE_TYPE("LOCKINGCLAUSE");

	MODWRITE_NODE_FIELD(lockedRels);
	MODWRITE_BOOL_FIELD(forUpdate);
	MODWRITE_BOOL_FIELD(nowait);
}

static void
mod_outColumnDef(StringInfo str, ColumnDef *node)
{
	MODWRITE_NODE_TYPE("COLUMNDEF");

	MODWRITE_STRING_FIELD(colname);
	MODWRITE_NODE_FIELD(typename);
	MODWRITE_INT_FIELD(inhcount);
	MODWRITE_BOOL_FIELD(is_local);
	MODWRITE_BOOL_FIELD(is_not_null);
	MODWRITE_NODE_FIELD(raw_default);
	MODWRITE_STRING_FIELD(cooked_default);
	MODWRITE_NODE_FIELD(constraints);
	MODWRITE_NODE_FIELD(support);
}

static void
mod_outTypeName(StringInfo str, TypeName *node)
{
	MODWRITE_NODE_TYPE("TYPENAME");

	MODWRITE_NODE_FIELD(names);
	MODWRITE_OID_FIELD(typeid);
	MODWRITE_BOOL_FIELD(timezone);
	MODWRITE_BOOL_FIELD(setof);
	MODWRITE_BOOL_FIELD(pct_type);
	MODWRITE_INT_FIELD(typmod);
	MODWRITE_NODE_FIELD(arrayBounds);
}

static void
mod_outTypeCast(StringInfo str, TypeCast *node)
{
	MODWRITE_NODE_TYPE("TYPECAST");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_NODE_FIELD(typename);
}

static void
mod_outIndexElem(StringInfo str, IndexElem *node)
{
	MODWRITE_NODE_TYPE("INDEXELEM");

	MODWRITE_STRING_FIELD(name);
	MODWRITE_NODE_FIELD(expr);
	MODWRITE_NODE_FIELD(opclass);
}

static void
mod_outVariableResetStmt(StringInfo str, VariableResetStmt *node)
{
	MODWRITE_NODE_TYPE("VARIABLERESETSTMT");
	MODWRITE_STRING_FIELD(name);
}

static void
mod_outQuery(StringInfo str, Query *node)
{
	MODWRITE_NODE_TYPE("QUERY");

	MODWRITE_ENUM_FIELD(commandType, CmdType);
	MODWRITE_ENUM_FIELD(querySource, QuerySource);
	MODWRITE_BOOL_FIELD(canSetTag);

	/*
	 * Hack to work around missing outfuncs routines for a lot of the
	 * utility-statement node types.  (The only one we actually *need* for
	 * rules support is NotifyStmt.)  Someday we ought to support 'em all, but
	 * for the meantime do this to avoid getting lots of warnings when running
	 * with debug_print_parse on.
	 */
	if (node->utilityStmt)
	{
		switch (nodeTag(node->utilityStmt))
		{
			case T_CreateStmt:
			case T_DropStmt:
			case T_TruncateStmt:
			case T_AlterTableStmt:
			case T_AlterTableCmd:
			case T_ViewStmt:
			case T_RuleStmt:
			
			case T_CreateRoleStmt:
			case T_AlterRoleStmt:
			case T_AlterRoleSetStmt:
			case T_DropRoleStmt:
			
			case T_CreateSchemaStmt:
			case T_CreatePLangStmt:
			case T_DropPLangStmt:
			case T_AlterOwnerStmt:
			
			case T_RenameStmt:
			case T_IndexStmt:
			case T_NotifyStmt:
			case T_DeclareCursorStmt:
			case T_VacuumStmt:
			case T_CreateSeqStmt:
			case T_CreatedbStmt:
			case T_AlterDatabaseSetStmt:
			case T_DropdbStmt:
			case T_CreateDomainStmt:
			case T_AlterDomainStmt:
			case T_ClusterStmt:
			
			case T_CreateFunctionStmt:
			case T_RemoveFuncStmt:
			
			case T_TransactionStmt:
			case T_CdbExecStmt:
			case T_GrantStmt:
			case T_LockStmt:
			case T_CopyStmt:
			case T_ReindexStmt:
			case T_VariableResetStmt:
				MODWRITE_NODE_FIELD(utilityStmt);
				break;
			default:
				appendStringInfoLiteral(str, " :utilityStmt ?");
				appendStringInfo(str, "%u", nodeTag(node->utilityStmt));
				if (Mpp_role == MPP_ROLE_DISPATCH)
				{
#ifdef _DEBUG
					elog(ERROR, "could not serialize unrecognized utilityStmt node type: %d",
						 (int) nodeTag(node->utilityStmt));
#else
					elog(WARNING, "could not serialize unrecognized utilityStmt node type: %d",
						 (int) nodeTag(node->utilityStmt));
#endif
				}
				break;
		}
	}
	else
		appendStringInfoLiteral(str, " :utilityStmt <>");

	MODWRITE_INT_FIELD(resultRelation);
	MODWRITE_NODE_FIELD(into);
	MODWRITE_BOOL_FIELD(hasAggs);
	MODWRITE_BOOL_FIELD(hasSubLinks);
	MODWRITE_NODE_FIELD(rtable);
	MODWRITE_NODE_FIELD(jointree);
	MODWRITE_NODE_FIELD(rowMarks);
	MODWRITE_BOOL_FIELD(forUpdate);
	MODWRITE_BOOL_FIELD(rowNoWait);
	MODWRITE_NODE_FIELD(targetList);
	MODWRITE_NODE_FIELD(groupClause);
	MODWRITE_NODE_FIELD(havingQual);
	MODWRITE_NODE_FIELD(distinctClause);
	MODWRITE_NODE_FIELD(sortClause);
	MODWRITE_NODE_FIELD(limitOffset);
	MODWRITE_NODE_FIELD(limitCount);
	MODWRITE_NODE_FIELD(setOperations);
	MODWRITE_NODE_FIELD(resultRelations);
}

static void
mod_outSortClause(StringInfo str, SortClause *node)
{
	MODWRITE_NODE_TYPE("SORTCLAUSE");

	MODWRITE_UINT_FIELD(tleSortGroupRef);
	MODWRITE_OID_FIELD(sortop);
}

static void
mod_outGroupClause(StringInfo str, GroupClause *node)
{
	MODWRITE_NODE_TYPE("GROUPCLAUSE");

	MODWRITE_UINT_FIELD(tleSortGroupRef);
	MODWRITE_OID_FIELD(sortop);
}

static void
mod_outSetOperationStmt(StringInfo str, SetOperationStmt *node)
{
	MODWRITE_NODE_TYPE("SETOPERATIONSTMT");

	MODWRITE_ENUM_FIELD(op, SetOperation);
	MODWRITE_BOOL_FIELD(all);
	MODWRITE_NODE_FIELD(larg);
	MODWRITE_NODE_FIELD(rarg);
	MODWRITE_NODE_FIELD(colTypes);
}

static void
mod_outRangeTblEntry(StringInfo str, RangeTblEntry *node)
{
	MODWRITE_NODE_TYPE("RTE");

	/* put alias + eref first to make dump more legible */
	MODWRITE_NODE_FIELD(alias);
	MODWRITE_NODE_FIELD(eref);
	MODWRITE_ENUM_FIELD(rtekind, RTEKind);

	switch (node->rtekind)
	{
		case RTE_RELATION:
		case RTE_SPECIAL:
			MODWRITE_OID_FIELD(relid);
			break;
		case RTE_SUBQUERY:
			MODWRITE_NODE_FIELD(subquery);
			break;
		case RTE_FUNCTION:
			MODWRITE_NODE_FIELD(funcexpr);
			MODWRITE_NODE_FIELD(coldeflist);
			break;
		case RTE_JOIN:
			MODWRITE_ENUM_FIELD(jointype, JoinType);
			MODWRITE_NODE_FIELD(joinaliasvars);
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) node->rtekind);
			break;
	}

	MODWRITE_BOOL_FIELD(inh);
	MODWRITE_BOOL_FIELD(inFromCl);
	MODWRITE_UINT_FIELD(requiredPerms);
	MODWRITE_OID_FIELD(checkAsUser);
}

static void
mod_outAExpr(StringInfo str, A_Expr *node)
{
	MODWRITE_NODE_TYPE("AEXPR");

	switch (node->kind)
	{
		case AEXPR_OP:
			appendStringInfoLiteral(str, " OPER ");
			MODWRITE_NODE_FIELD(name);
			break;
		case AEXPR_AND:
			appendStringInfoLiteral(str, " AND ");
			break;
		case AEXPR_OR:
			appendStringInfoLiteral(str, " OR ");
			break;
		case AEXPR_NOT:
			appendStringInfoLiteral(str, " NOT ");
			break;
		case AEXPR_OP_ANY:
			appendStringInfoLiteral(str, " ANY ");
			MODWRITE_NODE_FIELD(name);
		
			break;
		case AEXPR_OP_ALL:
			appendStringInfoLiteral(str, " ALL ");
			MODWRITE_NODE_FIELD(name);
		
			break;
		case AEXPR_DISTINCT:
			appendStringInfoLiteral(str, " DISTINCT ");
			MODWRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
			appendStringInfoLiteral(str, " NULLIF ");
			MODWRITE_NODE_FIELD(name);
			break;
		case AEXPR_OF:
			appendStringInfoLiteral(str, " OF ");
			MODWRITE_NODE_FIELD(name);
			break;
		default:
			appendStringInfoLiteral(str, " ??");
			break;
	}

	MODWRITE_NODE_FIELD(lexpr);
	MODWRITE_NODE_FIELD(rexpr);
}

static void
mod_outValue(StringInfo str, Value *value)
{
	switch (value->type)
	{
		case T_Integer:
			appendStringInfo(str, "%ld", value->val.ival);
			break;
		case T_Float:

			/*
			 * We assume the value is a valid numeric literal and so does not
			 * need quoting.
			 */
			appendStringInfoString(str, value->val.str);
			break;
		case T_String:
			appendStringInfoChar(str, '"');
			mod_outToken(str, value->val.str);
			appendStringInfoChar(str, '"');
			break;
		case T_BitString:
			/* internal representation already has leading 'b' */
			appendStringInfoString(str, value->val.str);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) value->type);
			break;
	}
}

static void
mod_outColumnRef(StringInfo str, ColumnRef *node)
{
	MODWRITE_NODE_TYPE("COLUMNREF");

	MODWRITE_NODE_FIELD(fields);
}

static void
mod_outParamRef(StringInfo str, ParamRef *node)
{
	MODWRITE_NODE_TYPE("PARAMREF");

	MODWRITE_INT_FIELD(number);
}

static void
mod_outAConst(StringInfo str, A_Const *node)
{
	MODWRITE_NODE_TYPE("A_CONST");

	appendStringInfoChar(str, ' ');

	mod_outValue(str, &(node->val));
	MODWRITE_NODE_FIELD(typename);
	
}

static void
mod_outA_Indices(StringInfo str, A_Indices *node)
{
	MODWRITE_NODE_TYPE("A_INDICES");

	MODWRITE_NODE_FIELD(lidx);
	MODWRITE_NODE_FIELD(uidx);
}

static void
mod_outA_Indirection(StringInfo str, A_Indirection *node)
{
	MODWRITE_NODE_TYPE("A_INDIRECTION");

	MODWRITE_NODE_FIELD(arg);
	MODWRITE_NODE_FIELD(indirection);
}

static void
mod_outResTarget(StringInfo str, ResTarget *node)
{
	MODWRITE_NODE_TYPE("RESTARGET");

	MODWRITE_STRING_FIELD(name);
	MODWRITE_NODE_FIELD(indirection);
	MODWRITE_NODE_FIELD(val);
}

static void
mod_outConstraint(StringInfo str, Constraint *node)
{
	MODWRITE_NODE_TYPE("CONSTRAINT");

	MODWRITE_STRING_FIELD(name);

	appendStringInfoLiteral(str, " :contype ");
	switch (node->contype)
	{
		case CONSTR_PRIMARY:
			appendStringInfoLiteral(str, "PRIMARY_KEY");
			MODWRITE_NODE_FIELD(keys);
			MODWRITE_STRING_FIELD(indexspace);
			break;

		case CONSTR_UNIQUE:
			appendStringInfoLiteral(str, "UNIQUE");
			MODWRITE_NODE_FIELD(keys);
			MODWRITE_STRING_FIELD(indexspace);
			break;

		case CONSTR_CHECK:
			appendStringInfoLiteral(str, "CHECK");
			MODWRITE_NODE_FIELD(raw_expr);
			MODWRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_DEFAULT:
			appendStringInfoLiteral(str, "DEFAULT");
			MODWRITE_NODE_FIELD(raw_expr);
			MODWRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_NOTNULL:
			appendStringInfoLiteral(str, "NOT_NULL");
			break;

		default:
			appendStringInfoLiteral(str, "<unrecognized_constraint>");
			break;
	}
}

static void
mod_outFkConstraint(StringInfo str, FkConstraint *node)
{
	MODWRITE_NODE_TYPE("FKCONSTRAINT");

	MODWRITE_STRING_FIELD(constr_name);
	MODWRITE_NODE_FIELD(pktable);
	MODWRITE_NODE_FIELD(fk_attrs);
	MODWRITE_NODE_FIELD(pk_attrs);
	MODWRITE_CHAR_FIELD(fk_matchtype);
	MODWRITE_CHAR_FIELD(fk_upd_action);
	MODWRITE_CHAR_FIELD(fk_del_action);
	MODWRITE_BOOL_FIELD(deferrable);
	MODWRITE_BOOL_FIELD(initdeferred);
	MODWRITE_BOOL_FIELD(skip_validation);
}

static void
mod_outCreateSchemaStmt(StringInfo str, CreateSchemaStmt *node)
{
	MODWRITE_NODE_TYPE("CREATESCHEMASTMT");

	MODWRITE_STRING_FIELD(schemaname);
	MODWRITE_STRING_FIELD(authid);
	MODWRITE_OID_FIELD(schemaOid);
	
}

static void
mod_outVacuumStmt(StringInfo str, VacuumStmt *node)
{
	MODWRITE_NODE_TYPE("VACUUMSTMT");

	MODWRITE_BOOL_FIELD(vacuum);
	MODWRITE_BOOL_FIELD(full);
	MODWRITE_BOOL_FIELD(analyze);
	MODWRITE_BOOL_FIELD(freeze);
	MODWRITE_BOOL_FIELD(verbose);
	MODWRITE_NODE_FIELD(relation);
	MODWRITE_NODE_FIELD(va_cols);
}

static void
mod_outRemoteParamData(StringInfo str, RemoteParamData *node)
{
	int16 typlen;
	bool typbyval;

	get_typlenbyval(node->paramData.ptype, &typlen, &typbyval);

	MODWRITE_NODE_TYPE("REMOTEPARAMDATA");
	MODWRITE_INT_FIELD(paramData.kind);
	MODWRITE_STRING_FIELD(paramData.name);
	MODWRITE_INT_FIELD(paramData.id);
	MODWRITE_OID_FIELD(paramData.ptype);

	if ( !typbyval && typlen < 0 && node->paramData.value == 0 )
		node->paramData.isnull = true;

	MODWRITE_BOOL_FIELD(paramData.isnull);

	appendStringInfoLiteral(str, " :value ");

	/*
	 * When the value is NULL for a datum that is not a typbyval,
	 * and the typlen=-1 (say, for a NULL text datum),
	 * the mod_outDatum call would call ereport when it tried to
	 * call datumGetSize.  So we must test for null here, and
	 * avoid calling mod_outDatum for a NULL value.
	 */
	if ( node->paramData.isnull )
		appendStringInfoLiteral(str, "<>");
	else
		mod_outDatum(str, node->paramData.value, typlen, typbyval);
}

static void
mod_outCdbProcess(StringInfo str, CdbProcess *node)
{
	MODWRITE_NODE_TYPE("CDBPROCESS");
	MODWRITE_STRING_FIELD(listenerAddr);
	MODWRITE_INT_FIELD(listenerPort);
	MODWRITE_INT_FIELD(pid);
	MODWRITE_INT_FIELD(contentid);
	MODWRITE_INT_FIELD(routeid);
}

static void
mod_outSlice(StringInfo str, Slice *node)
{
	MODWRITE_NODE_TYPE("SLICE");
	MODWRITE_INT_FIELD(sliceIndex);
	MODWRITE_INT_FIELD(rootIndex);
	MODWRITE_ENUM_FIELD(gangType,GangType);
	MODWRITE_INT_FIELD(gangSize);
	MODWRITE_DUMMY_FIELD(primaryGang);
	MODWRITE_DUMMY_FIELD(mirrorGang);
	MODWRITE_INT_FIELD(parentIndex);
	MODWRITE_NODE_FIELD(children); /* List of int index */
	MODWRITE_NODE_FIELD(primaryProcesses); /* List of (CDBProcess *) */
	MODWRITE_NODE_FIELD(mirrorProcesses); /* List of (CDBProcess *) */
}

static void
mod_outSliceTable(StringInfo str, SliceTable *node)
{
	MODWRITE_NODE_TYPE("SLICETABLE");
	MODWRITE_INT_FIELD(nMotions);
	MODWRITE_INT_FIELD(nextInitPlan);
	MODWRITE_INT_FIELD(nSubplans);
	MODWRITE_INT_FIELD(localSlice);
	MODWRITE_NODE_FIELD(slices); /* List of int */
}

static void
mod_outSnapshotInfo(StringInfo str, SnapshotInfo *node)
{
	int i;
	
	MODWRITE_NODE_TYPE("SNAPSHOTINFO");
	
	MODWRITE_UINT_FIELD(currentTransactionId);
	MODWRITE_UINT_FIELD(xmin);
	MODWRITE_UINT_FIELD(xmax);
	MODWRITE_INT_FIELD(xcnt);
	
	appendStringInfoLiteral(str, " :xip");
	for( i = 0; i < node->xcnt; i++ )
		appendStringInfo(str, " %u", node->xip[i]);
	
	MODWRITE_UINT_FIELD(curcid);
}


/*
 * mod_outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
static void
mod_outNode(StringInfo str, void *obj)
{
	if (obj == NULL)
		appendStringInfoLiteral(str, "<>");
	else if (IsA(obj, List) ||IsA(obj, IntList) || IsA(obj, OidList))
		mod_outList(str, obj);
	else if (IsA(obj, Integer) ||
			 IsA(obj, Float) ||
			 IsA(obj, String) ||
			 IsA(obj, BitString))
	{
		/* nodeRead does not want to see { } around these! */
		mod_outValue(str, obj);
	}
	else
	{
		appendStringInfoChar(str, '{');
		switch (nodeTag(obj))
		{
			case T_Plan:
				mod_outPlan(str, obj);
				break;
			case T_Result:
				mod_outResult(str, obj);
				break;
			case T_Append:
				mod_outAppend(str, obj);
				break;
			case T_BitmapAnd:
				mod_outBitmapAnd(str, obj);
				break;
			case T_BitmapOr:
				mod_outBitmapOr(str, obj);
				break;
			case T_Scan:
				mod_outScan(str, obj);
				break;
			case T_SeqScan:
				mod_outSeqScan(str, obj);
				break;
			case T_IndexScan:
				mod_outIndexScan(str, obj);
				break;
			case T_BitmapIndexScan:
				mod_outBitmapIndexScan(str, obj);
				break;
			case T_BitmapHeapScan:
				mod_outBitmapHeapScan(str, obj);
				break;
			case T_TidScan:
				mod_outTidScan(str, obj);
				break;
			case T_SubqueryScan:
				mod_outSubqueryScan(str, obj);
				break;
			case T_FunctionScan:
				mod_outFunctionScan(str, obj);
				break;
			case T_Join:
				mod_outJoin(str, obj);
				break;
			case T_NestLoop:
				mod_outNestLoop(str, obj);
				break;
			case T_MergeJoin:
				mod_outMergeJoin(str, obj);
				break;
			case T_HashJoin:
				mod_outHashJoin(str, obj);
				break;
			case T_Agg:
				mod_outAgg(str, obj);
				break;
			case T_Group:
				mod_outGroup(str, obj);
				break;
			case T_Material:
				mod_outMaterial(str, obj);
				break;
			case T_Sort:
				mod_outSort(str, obj);
				break;
			case T_Unique:
				mod_outUnique(str, obj);
				break;
			case T_SetOp:
				mod_outSetOp(str, obj);
				break;
			case T_Limit:
				mod_outLimit(str, obj);
				break;
			case T_Hash:
				mod_outHash(str, obj);
				break;
			case T_Motion:
				mod_outMotion(str, obj);
				break;
			case T_Alias:
				mod_outAlias(str, obj);
				break;
			case T_RangeVar:
				mod_outRangeVar(str, obj);
				break;
			case T_Var:
				mod_outVar(str, obj);
				break;
			case T_Const:
				mod_outConst(str, obj);
				break;
			case T_Param:
				mod_outParam(str, obj);
				break;
			case T_Aggref:
				mod_outAggref(str, obj);
				break;
			case T_ArrayRef:
				mod_outArrayRef(str, obj);
				break;
			case T_FuncExpr:
				mod_outFuncExpr(str, obj);
				break;
			case T_OpExpr:
				mod_outOpExpr(str, obj);
				break;
			case T_DistinctExpr:
				mod_outDistinctExpr(str, obj);
				break;
			case T_ScalarArrayOpExpr:
				mod_outScalarArrayOpExpr(str, obj);
				break;
			case T_BoolExpr:
				mod_outBoolExpr(str, obj);
				break;
			case T_SubLink:
				mod_outSubLink(str, obj);
				break;
			case T_SubPlan:
				mod_outSubPlan(str, obj);
				break;
			case T_FieldSelect:
				mod_outFieldSelect(str, obj);
				break;
			case T_FieldStore:
				mod_outFieldStore(str, obj);
				break;
			case T_RelabelType:
				mod_outRelabelType(str, obj);
				break;
			case T_ConvertRowtypeExpr:
				mod_outConvertRowtypeExpr(str, obj);
				break;
			case T_CaseExpr:
				mod_outCaseExpr(str, obj);
				break;
			case T_CaseWhen:
				mod_outCaseWhen(str, obj);
				break;
			case T_CaseTestExpr:
				mod_outCaseTestExpr(str, obj);
				break;
			case T_ArrayExpr:
				mod_outArrayExpr(str, obj);
				break;
			case T_RowExpr:
				mod_outRowExpr(str, obj);
				break;
			case T_CoalesceExpr:
				mod_outCoalesceExpr(str, obj);
				break;
			case T_MinMaxExpr:
				mod_outMinMaxExpr(str, obj);
				break;
			case T_NullIfExpr:
				mod_outNullIfExpr(str, obj);
				break;
			case T_NullTest:
				mod_outNullTest(str, obj);
				break;
			case T_BooleanTest:
				mod_outBooleanTest(str, obj);
				break;
			case T_CoerceToDomain:
				mod_outCoerceToDomain(str, obj);
				break;
			case T_CoerceToDomainValue:
				mod_outCoerceToDomainValue(str, obj);
				break;
			case T_SetToDefault:
				mod_outSetToDefault(str, obj);
				break;
			case T_TargetEntry:
				mod_outTargetEntry(str, obj);
				break;
			case T_RangeTblRef:
				mod_outRangeTblRef(str, obj);
				break;
			case T_JoinExpr:
				mod_outJoinExpr(str, obj);
				break;
			case T_FromExpr:
				mod_outFromExpr(str, obj);
				break;
			case T_Flow:
				mod_outFlow(str, obj);
				break;
			case T_RemoteParamData:
				mod_outRemoteParamData(str, obj);
				break;

			case T_Path:
				mod_outPath(str, obj);
				break;
			case T_IndexPath:
				mod_outIndexPath(str, obj);
				break;
			case T_BitmapHeapPath:
				mod_outBitmapHeapPath(str, obj);
				break;
			case T_BitmapAndPath:
				mod_outBitmapAndPath(str, obj);
				break;
			case T_BitmapOrPath:
				mod_outBitmapOrPath(str, obj);
				break;
			case T_TidPath:
				mod_outTidPath(str, obj);
				break;
			case T_AppendPath:
				mod_outAppendPath(str, obj);
				break;
			case T_ResultPath:
				mod_outResultPath(str, obj);
				break;
			case T_MaterialPath:
				mod_outMaterialPath(str, obj);
				break;
			case T_UniquePath:
				mod_outUniquePath(str, obj);
				break;
			case T_NestPath:
				mod_outNestPath(str, obj);
				break;
			case T_MergePath:
				mod_outMergePath(str, obj);
				break;
			case T_HashPath:
				mod_outHashPath(str, obj);
				break;
            case T_CdbMotionPath:
                mod_outCdbMotionPath(str, obj);
                break;
			case T_PlannerInfo:
				mod_outPlannerInfo(str, obj);
				break;
			case T_RelOptInfo:
				mod_outRelOptInfo(str, obj);
				break;
			case T_IndexOptInfo:
				mod_outIndexOptInfo(str, obj);
				break;
			case T_PathKeyItem:
				mod_outPathKeyItem(str, obj);
				break;
			case T_RestrictInfo:
				mod_outRestrictInfo(str, obj);
				break;
			case T_InnerIndexscanInfo:
				mod_outInnerIndexscanInfo(str, obj);
				break;
			case T_InClauseInfo:
				mod_outInClauseInfo(str, obj);
				break;
				
			case T_GrantStmt:
				mod_outGrantStmt(str, obj);
				break;
			case T_PrivGrantee:
				mod_outPrivGrantee(str, obj);
				break;
			case T_FuncWithArgs:
				mod_outFuncWithArgs(str, obj);
				break;
			case T_LockStmt:
				mod_outLockStmt(str, obj);
				break;

			case T_CreateStmt:
				mod_outCreateStmt(str, obj);
				break;
			case T_IndexStmt:
				mod_outIndexStmt(str, obj);
				break;
			case T_ReindexStmt:
				mod_outReindexStmt(str, obj);
				break;
				
			case T_CreateFunctionStmt:
				mod_outCreateFunctionStmt(str, obj);
				break;
			case T_FunctionParameter:
				mod_outFunctionParameter(str, obj);
				break;
			case T_RemoveFuncStmt:
				mod_outRemoveFuncStmt(str, obj);
				break;
			
				
			case T_ViewStmt:
				mod_outViewStmt(str, obj);
				break;
			case T_RuleStmt:
				mod_outRuleStmt(str, obj);
				break;
			case T_DropStmt:
				mod_outDropStmt(str, obj);
				break;
			case T_TruncateStmt:
				mod_outTruncateStmt(str, obj);
				break;

			case T_AlterTableStmt:
				mod_outAlterTableStmt(str, obj);
				break;
			case T_AlterTableCmd:
				mod_outAlterTableCmd(str, obj);
				break;

			case T_CreateRoleStmt:
				mod_outCreateRoleStmt(str, obj);
				break;
			case T_DropRoleStmt:
				mod_outDropRoleStmt(str, obj);
				break;
			case T_AlterRoleStmt:
				mod_outAlterRoleStmt(str, obj);
				break;
			case T_AlterRoleSetStmt:
				mod_outAlterRoleSetStmt(str, obj);
				break;

			case T_RenameStmt:
				mod_outRenameStmt(str, obj);
				break;

			case T_CreateSeqStmt:
				mod_outCreateSeqStmt(str, obj);
				break;
			case T_CreatedbStmt:
				mod_outCreatedbStmt(str, obj);
				break;
			case T_CreateDomainStmt:
				mod_outCreateDomainStmt(str, obj);
				break;
			case T_AlterDomainStmt:
				mod_outAlterDomainStmt(str, obj);
				break;

			case T_TransactionStmt:
				mod_outTransactionStmt(str, obj);
				break;

			case T_NotifyStmt:
				mod_outNotifyStmt(str, obj);
				break;
			case T_DeclareCursorStmt:
				mod_outDeclareCursorStmt(str, obj);
				break;
			case T_CopyStmt:
				mod_outCopyStmt(str, obj);
				break;
			case T_CdbExecStmt:
				mod_outCdbExecStmt(str, obj);
				break;
			case T_SelectStmt:
				mod_outSelectStmt(str, obj);
				break;
			case T_ColumnDef:
				mod_outColumnDef(str, obj);
				break;
			case T_TypeName:
				mod_outTypeName(str, obj);
				break;
			case T_TypeCast:
				mod_outTypeCast(str, obj);
				break;
			case T_IndexElem:
				mod_outIndexElem(str, obj);
				break;
			case T_Query:
				mod_outQuery(str, obj);
				break;
			case T_SortClause:
				mod_outSortClause(str, obj);
				break;
			case T_GroupClause:
				mod_outGroupClause(str, obj);
				break;
			case T_SetOperationStmt:
				mod_outSetOperationStmt(str, obj);
				break;
			case T_RangeTblEntry:
				mod_outRangeTblEntry(str, obj);
				break;
			case T_A_Expr:
				mod_outAExpr(str, obj);
				break;
			case T_ColumnRef:
				mod_outColumnRef(str, obj);
				break;
			case T_ParamRef:
				mod_outParamRef(str, obj);
				break;
			case T_A_Const:
				mod_outAConst(str, obj);
				break;
			case T_A_Indices:
				mod_outA_Indices(str, obj);
				break;
			case T_A_Indirection:
				mod_outA_Indirection(str, obj);
				break;
			case T_ResTarget:
				mod_outResTarget(str, obj);
				break;
			case T_Constraint:
				mod_outConstraint(str, obj);
				break;
			case T_FkConstraint:
				mod_outFkConstraint(str, obj);
				break;
			case T_FuncCall:
				mod_outFuncCall(str, obj);
				break;
			case T_DefElem:
				mod_outDefElem(str, obj);
				break;
			case T_CreateSchemaStmt:
				mod_outCreateSchemaStmt(str, obj);
				break;
			case T_VacuumStmt:
				mod_outVacuumStmt(str, obj);
				break;			
			case T_CdbProcess:
				mod_outCdbProcess(str, obj);
				break;
			case T_Slice:
				mod_outSlice(str, obj);
				break;
			case T_SliceTable:
				mod_outSliceTable(str, obj);
				break;
			case T_SnapshotInfo:
				mod_outSnapshotInfo(str, obj);
				break;
			case T_VariableResetStmt:
				mod_outVariableResetStmt(str, obj);
				break;
				
			case T_LockingClause:
				mod_outLockingClause(str, obj);
				break;

			default:

				/*
				 * This should be an ERROR, but it's too useful to be able to
				 * dump structures that mod_outNode only understands part of.
				 */
				if (Mpp_role == MPP_ROLE_DISPATCH)
					elog(ERROR, "could not serialize unrecognized node type: %d",
						 (int) nodeTag(obj));
				else
				
					elog(WARNING, "could not dump unrecognized node type: %d",
						 (int) nodeTag(obj));
				break;
		}
		appendStringInfoChar(str, '}');
	}
}



/*
 * ModifiedNodeToString -
 *	   returns the ascii representation of the Node as a palloc'd string
 */
char *
ModifiedNodeToString(void *obj)
{
	StringInfoData str;
	int bufsize = 8*1024;
 
	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfoOfSize(&str, bufsize);
	mod_outNode(&str, obj);
	return str.data;
}



static char *
EscapeQuotes(const char *src, int *size)
{
        int                     len = strlen(src),
                                i,
                                j;
        char       *result = palloc(len * 2 + 1);

        for (i = 0, j = 0; i < len; i++)
        {
                if (src[i] == '\'' || src[i] == '\\')
                        result[j++] = '\\';
                result[j++] = src[i];
        }

        if (size != NULL)
                *size = j;

        result[j] = '\0';
        return result;
}

char *
serializeThis(Node *node, int *size)
{
        char       *pszNode;
        char       *sNode;

        Assert(node != NULL);

        pszNode = ModifiedNodeToString(node);
        Assert(pszNode != NULL);

        sNode = EscapeQuotes(pszNode, size);
        pfree(pszNode);

        return sNode;
}
