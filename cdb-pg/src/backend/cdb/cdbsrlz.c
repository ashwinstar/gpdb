/*-------------------------------------------------------------------------
 *
 * cdbsrlz.c
 *	  Serialize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2004, Greenplum
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <regex.h>
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbsrlz.h"

/*
 * The following code was developed to test xml serialization.
 * We can delete it all later if it turns out we don't need it.
 * For now, we'll just get rid of it with an #if 0
 */
#if 0
/*
#define SERIALIZE_INT_FIELD(fldname)   (_serializeIntField(context, (Node *)node, (int *)&node->fldname, CppAsString(fldname) ))

#define SERIALIZE_BOOL_FIELD(fldname)	(_serializeBoolField(context, (Node *)node, (bool *)&node->fldname, CppAsString(fldname) ))

#define SERIALIZE_UINT_FIELD(fldname)	(_serializeUIntField(context, (Node *)node, (uint *)&node->fldname, CppAsString(fldname) ))

#define SERIALIZE_OID_FIELD(fldname)   SERIALIZE_UINT_FIELD(fldname)

#define SERIALIZE_STRING_FIELD(fldname)   (_serializeStringField(context, (Node *)node, (char **)&node->fldname, CppAsString(fldname) ))

#define SERIALIZE_DOUBLE_FIELD(fldname)   (_serializeDoubleField(context, (Node *)node, (double *)&node->fldname, CppAsString(fldname) ))

#define SERIALIZE_COST_FIELD(fldname)	  SERIALIZE_DOUBLE_FIELD(fldname)

#define SERIALIZE_BITMAPSET_FIELD(fldname) (_serializeBitmapsetField(context, (Node *)node, (Bitmapset **)&node->fldname, CppAsString(fldname) ))

#define SERIALIZE_INDEX_FIELD(fldname)	  SERIALIZE_UINT_FIELD(fldname)

#define SERIALIZE_DATUM(fldname, len, byval)	  SERIALIZE_UINT_FIELD(fldname)

#define SERIALIZE_NODE_FIELD(fldname)	 (serialize_node((Node **)&node->fldname, CppAsString(fldname), context))


static void startTag(SerializeContext * context, const char *Tag);
static void startNodeTag(SerializeContext * context, const char *Tag, int nodeType);
static void endTag(SerializeContext * context);
static void setValue(SerializeContext * context, const char *fmt,...);
static TagStack *createTagStack(void);
static void destroyTagStack(TagStack * stack);
static void pushTagStack(TagStack * stack, const char *tag);
static const char *popTagStack(TagStack * stack);
static void serialize_node(Node **pNode, char *pszTagName, SerializeContext * context);
static void serializeScanFields(Scan *node, SerializeContext * context);
static void serializePlanFields(Plan *node, SerializeContext * context);
static void serializeVarFields(Var *node, SerializeContext * context);
static void serializeTargetEntryFields(TargetEntry *node, SerializeContext * context);
static void serializeOpExprFields(OpExpr *node, SerializeContext * context);
static void serializeConstFields(Const *node, SerializeContext * context);
static void serializeDatum(Datum value, char *pszTag, int typlen, bool typbyval, SerializeContext * context);
static void _serializeConstantField(SerializeContext * context, char *value, char *pszFldName);
static void _serializeIntField(SerializeContext * context, Node *node, int *intfld, char *pszFldName);
static void _serializeUIntField(SerializeContext * context, Node *node, uint *uintfld, char *pszFldName);
static void _serializeBoolField(SerializeContext * context, Node *node, bool *boolfld, char *pszFldName);
static void _serializeDoubleField(SerializeContext * context, Node *node, double *dblfld, char *pszFldName);
static void _serializeStringField(SerializeContext * context, Node *node, char **strfld, char *pszFldName);
static void _serializeBitmapsetField(SerializeContext * context, Node *node, Bitmapset **fld, char *pszFldName);

static int	findChar(SerializeContext * context, char c);
static void matchChar(SerializeContext * context, char c);
static bool checkChar(SerializeContext * context, char c);
static void matchString(SerializeContext * context, char *s);
static bool checkString(SerializeContext * context, char *s);
static void skipWhitespace(SerializeContext * context);
static void bumpOffset(SerializeContext * context, int n);
static int	matchNextNodeTag(SerializeContext * context, char *pszTagName, bool *bIsNull);
static void matchNextTag(SerializeContext * context, char *pszTagName);
static void matchNextEndTag(SerializeContext * context, char *pszTagName);
static bool checkNextEndTag(SerializeContext * context, char *pszTagName);

static int	getIntValue(SerializeContext * context);
static uint getUIntValue(SerializeContext * context);
static double getDoubleValue(SerializeContext * context);
static bool getBoolValue(SerializeContext * context);
static char *getStringValue(SerializeContext * context);
*/
/*
 * nodes/outfuncs.c
 */
//extern void outDatum(StringInfo str, Datum value, int typlen, bool typbyval);

/* ----------------------------------------------------------------------- *
 * Functions serializePlan and	serialize_plan_walker() use the plan_tree_walker()
 * framework to visit the nodes of a plan tree to be serialized.
 * The walker may also choose to exit with an error, usually to report an unsupported
 * feature used in the plan tree or an unexpected problem.
 *
 * Returns true if the serialization succeeds, false if it fails.
 *
 * In case the result is false, the caller should issue an error.
 * ----------------------------------------------------------------------- *
 */
 /*
void
serializePlan(Plan *plan, SerializeContext * context)
{

	initStringInfo(&context->result);
	context->tagStack = createTagStack();
	context->fromNode = true;
	context->input = NULL;
	context->offset = 0;
	context->eof = false;

	serialize_node((Node **) &plan, "Plan", context);
}



bool
deSerializePlan(Plan **pplan, SerializeContext * context, char *szInput)
{

	initStringInfo(&context->result);
	context->tagStack = createTagStack();
	context->fromNode = false;
	context->input = szInput;
	context->offset = 0;
	context->eof = false;

	serialize_node((Node **) pplan, "Plan", context);

	return true;
}

void
_serializeConstantField(SerializeContext * context, char *value, char *pszFldName)
{
	Assert(context->fromNode);
	Assert(value != NULL);

	if (context->fromNode)
	{
		startTag(context, pszFldName);
		setValue(context, "%s", value);
		endTag(context);
	}
}

void
_serializeIntField(SerializeContext * context, Node *node, int *intfld, char *pszFldName)
{
	Assert(intfld != NULL);

	if (context->fromNode)
	{
		startTag(context, pszFldName);
		setValue(context, "%d", *intfld);
		endTag(context);
	}
	else
	{
		matchNextTag(context, pszFldName);
		*intfld = getIntValue(context);
		matchNextEndTag(context, pszFldName);
	}
}

void
_serializeUIntField(SerializeContext * context, Node *node, uint *uintfld, char *pszFldName)
{
	Assert(uintfld != NULL);

	if (context->fromNode)
	{
		startTag(context, pszFldName);
		setValue(context, "%u", *uintfld);
		endTag(context);
	}
	else
	{
		matchNextTag(context, pszFldName);
		*uintfld = getUIntValue(context);
		matchNextEndTag(context, pszFldName);
	}
}

void
_serializeBoolField(SerializeContext * context, Node *node, bool *boolfld, char *pszFldName)
{
	Assert(boolfld != NULL);

	if (context->fromNode)
	{
		startTag(context, pszFldName);
		setValue(context, "%s", ((*boolfld) ? "1" : "0"));
		endTag(context);
	}
	else
	{
		matchNextTag(context, pszFldName);
		*boolfld = getBoolValue(context);
		matchNextEndTag(context, pszFldName);
	}
}

void
_serializeDoubleField(SerializeContext * context, Node *node, double *dblfld, char *pszFldName)
{
	Assert(dblfld != NULL);

	if (context->fromNode)
	{
		startTag(context, pszFldName);
		setValue(context, "%lf", *dblfld);
		endTag(context);
	}
	else
	{
		matchNextTag(context, pszFldName);
		*dblfld = getDoubleValue(context);
		matchNextEndTag(context, pszFldName);
	}
}

void
_serializeStringField(SerializeContext * context, Node *node, char **strfld, char *pszFldName)
{
	Assert(strfld != NULL);

	if (context->fromNode)
	{
		startTag(context, pszFldName);
		if (*strfld == NULL)
			setValue(context, "%s", "NULL");
		else
			setValue(context, "%s", *strfld);
		endTag(context);
	}
	else
	{
		matchNextTag(context, pszFldName);
		*strfld = getStringValue(context);
		matchNextEndTag(context, pszFldName);
	}
}

void
_serializeBitmapsetField(SerializeContext * context, Node *node, Bitmapset **fld, char *pszFldName)
{
	Assert(fld != NULL);

	if (context->fromNode)
	{
		startTag(context, pszFldName);
		setValue(context, "%s", "NULL");
		endTag(context);
	}
	else
	{
		matchNextTag(context, pszFldName);
		getStringValue(context);
		*fld = NULL;
		matchNextEndTag(context, pszFldName);
	}
}

TagStack *
createTagStack(void)
{
	TagStack   *stack = palloc0(sizeof(TagStack));

	stack->capacity = 256;

	stack->tags = (char **) palloc0(stack->capacity * sizeof(char *));

	return stack;
}

void
destroyTagStack(TagStack * stack)
{
	if (stack == NULL)
		return;

	if (stack->tags != NULL)
	{
		int			i;

		for (i = 0; i < stack->size; i++)
		{
			if (stack->tags[i] != NULL)
				pfree(stack->tags[i]);
		}
		pfree(stack->tags);
		stack->tags = NULL;
	}

	pfree(stack);
}

void
pushTagStack(TagStack * stack, const char *tag)
{
	if (stack->size >= stack->capacity)
	{
		int			old_capacity = stack->capacity;

		stack->tags = (char **) repalloc(stack->tags, 2 * old_capacity * sizeof(char *));
		memset(&stack->tags[old_capacity], 0, old_capacity * sizeof(char *));

		stack->capacity *= 2;
	}

	stack->tags[stack->size++] = pstrdup(tag);
}

const char *
popTagStack(TagStack * stack)
{
	Assert(stack->size > 0);
	const char *value = stack->tags[stack->size - 1];

	stack->tags[stack->size - 1] = NULL;
	stack->size--;
	return value;
}

void
destroySerializeContext(SerializeContext * context)
{
	if (context == NULL)
		return;

	pfree(context->result.data);
	context->result.data = NULL;
	if (context->tagStack != NULL)
	{
		destroyTagStack(context->tagStack);
		context->tagStack = NULL;
	}
}
*/

/*
 * Function serialize_plan_walker is the workhorse of serializePlan.
 *
 */
 /*
void
serialize_node(Node **pNode, char *pszTagName, SerializeContext * context)
{
	const char *detail = NULL;
	ListCell   *temp;


		Node	   *node;
	int			tag;
	bool		fromNode = context->fromNode;

	Assert(pNode != NULL);

	if (fromNode)
	{
		node = *pNode;

		if (node == NULL)
		{
			_serializeConstantField(context, "NULL", pszTagName);
			return;
		}

		tag = nodeTag(node);
		startNodeTag(context, pszTagName, nodeTag(node));

	}
	else
	{
		bool		bIsNull = false;

		tag = matchNextNodeTag(context, pszTagName, &bIsNull);
		if (bIsNull)
		{
			*pNode = NULL;
			matchNextEndTag(context, pszTagName);
			return;
		}
	}

	switch (tag)
	{
		case T_List:
			if (!fromNode)
			{
				List	   *l = NIL;

				while (!checkNextEndTag(context, pszTagName))
				{
					Node	   *pListNode = NULL;

					serialize_node(&pListNode, "Node", context);
					l = lappend(l, pListNode);
				}


				*pNode = (Node *) l;
			}
			else
			{
				foreach(temp, (List *) node)
				{

						Node	   *listnode = (Node *) lfirst(temp);


						serialize_node(&listnode, "Node", context);

				}
			}
				break;

		case T_SeqScan:
			if (!fromNode)
			{
				node = (Node *) makeNode(SeqScan);
				*pNode = node;
			}
			serializeScanFields((Scan *) node, context);
			break;
			
		case T_BitmapHeapScan:
			if (!fromNode)
			{
				node = (Node *) makeNode(BitmapHeapScan);
				*pNode = node;
			}
			serializeScanFields((Scan *) node, context);
			break;

		case T_TargetEntry:
			if (!fromNode)
			{
				node = (Node *) makeNode(TargetEntry);
				*pNode = node;
			}
			serializeTargetEntryFields((TargetEntry *) node, context);
			break;

		case T_Var:
			if (!fromNode)
			{
				node = (Node *) makeNode(Var);
				*pNode = node;
			}
			serializeVarFields((Var *) node, context);
			break;

		case T_OpExpr:
			if (!fromNode)
			{
				node = (Node *) makeNode(OpExpr);
				*pNode = node;
			}
			serializeOpExprFields((OpExpr *) node, context);
			break;

		case T_Const:
			if (!fromNode)
			{
				node = (Node *) makeNode(Const);
				*pNode = node;
			}
			serializeConstFields((Const *) node, context);
			break;

		default:
			elog(NOTICE, "Partial serialized plan dump: %s", context->result.data);

			ereport(ERROR,
					(errcode(ERRCODE_CDB_INTERNAL_ERROR),
					 errmsg("Sorry, node type %d hasn't been implemented in serialize_node.", nodeTag(node))));

	}

	if (fromNode)
		endTag(context);
	else
		matchNextEndTag(context, pszTagName);

}

void
serializePlanFields(Plan *node, SerializeContext * context)
{
	SERIALIZE_COST_FIELD(startup_cost);
	SERIALIZE_COST_FIELD(total_cost);
	SERIALIZE_DOUBLE_FIELD(plan_rows);
	SERIALIZE_INT_FIELD(plan_width);
	SERIALIZE_NODE_FIELD(targetlist);

		SERIALIZE_NODE_FIELD(qual);

		SERIALIZE_NODE_FIELD(lefttree);

		SERIALIZE_NODE_FIELD(righttree);

		SERIALIZE_NODE_FIELD(initPlan);

		SERIALIZE_BITMAPSET_FIELD(extParam);
	SERIALIZE_BITMAPSET_FIELD(allParam);
	SERIALIZE_INT_FIELD(nParamExec);
	SERIALIZE_NODE_FIELD(flow);

}

void
serializeScanFields(Scan *node, SerializeContext * context)
{
	serializePlanFields((Plan *) node, context);
	SERIALIZE_INDEX_FIELD(scanrelid);
}


void
startTag(SerializeContext * context, const char *Tag)
{
	Assert(Tag != NULL);
	appendStringInfo(&context->result, "<%s>", Tag);
	pushTagStack(context->tagStack, Tag);
}

void
startNodeTag(SerializeContext * context, const char *Tag, int nodeType)
{
	Assert(Tag != NULL);
	appendStringInfo(&context->result, "<%s type=\"%d\">", Tag, nodeType);
	pushTagStack(context->tagStack, Tag);
}

void
endTag(SerializeContext * context)
{
	const char *Tag = popTagStack(context->tagStack);

	Assert(Tag != NULL);

	appendStringInfo(&context->result, "</%s>", Tag);
}

void
setValue(SerializeContext * context, const char *fmt,...)
{
	for (;;)
	{
		va_list		args;
		bool		success;

		
		va_start(args, fmt);
		success = appendStringInfoVA(&context->result, fmt, args);
		va_end(args);

		if (success)
			break;

	
		enlargeStringInfo(&context->result, context->result.maxlen);
	}
}

void
serializeVarFields(Var *node, SerializeContext * context)
{
	SERIALIZE_UINT_FIELD(varno);
	SERIALIZE_INT_FIELD(varattno);
	SERIALIZE_OID_FIELD(vartype);
	SERIALIZE_INT_FIELD(vartypmod);
	SERIALIZE_UINT_FIELD(varlevelsup);
	SERIALIZE_UINT_FIELD(varnoold);
	SERIALIZE_INT_FIELD(varoattno);
}

void
serializeTargetEntryFields(TargetEntry *node, SerializeContext * context)
{
	SERIALIZE_NODE_FIELD(expr);
}

void
serializeOpExprFields(OpExpr *node, SerializeContext * context)
{
	SERIALIZE_OID_FIELD(opno);
	SERIALIZE_OID_FIELD(opfuncid);
	SERIALIZE_OID_FIELD(opresulttype);
	SERIALIZE_BOOL_FIELD(opretset);
	SERIALIZE_NODE_FIELD(args);
}

void
serializeConstFields(Const *node, SerializeContext * context)
{
	SERIALIZE_OID_FIELD(consttype);
	SERIALIZE_INT_FIELD(constlen);
	SERIALIZE_BOOL_FIELD(constbyval);
	SERIALIZE_BOOL_FIELD(constisnull);

	if (node->constisnull)
	{
		_serializeConstantField(context, "NULL", "constvalue");
	}
	else
	{
		serializeDatum(node->constvalue, "constvalue", node->constlen, node->constbyval, context);
	}
}


void
serializeDatum(Datum value, char *pszTag, int typlen, bool typbyval, SerializeContext * context)
{
	if (context->fromNode)
	{
		StringInfoData str;

		initStringInfo(&str);
		outDatum(&str, value, typlen, typbyval);

		_serializeConstantField(context, str.data, pszTag);

		pfree(str.data);
	}
	else
	{

		matchNextTag(context, pszTag);

		 *
		 * We save and restore the pre-existing state of pg_strtok. This
		 * makes the world safe for re-entrant invocation of stringToNode,
		 * without incurring a lot of notational overhead by having to pass
		 * the next-character pointer around through all the readfuncs.c
		 * code.
		 *
			if (typbyval)

		{

			 * Read the length
				skipWhitespace(context);

				int			index = findChar(context, '[');


				context->input[index] = '\0';

				int			length = atoi(context->input + context->offset);


				context->input[index] = '[';

				bumpOffset(context, index - context->offset + 1);


				value = (Datum) 0;

				char	   *s = (char *) &value;


				int			i;


				for (i = 0; i < sizeof(Datum); i++)

			{

					skipWhitespace(context);

					index = findChar(context, ' ');

					context->input[index] = '\0';

					s[i] = (char) atoi(context->input + context->offset);

					context->input[index] = ' ';

					bumpOffset(context, index - context->offset + 1);

			}

				skipWhitespace(context);

				matchChar(context, ']');

		}
			else

		{

				ereport(ERROR,
						(errcode(ERRCODE_CDB_INTERNAL_ERROR),
						 errmsg("Sorry, deserialization of by reference Datums hasn't been implemented yet")));
		}

			matchNextEndTag(context, pszTag);
	}
}


int
matchNextNodeTag(SerializeContext * context, char *pszTagName, bool *bIsNull)
{
	Assert(context->input != NULL && context->offset < strlen(context->input));

	 * Should be positioned on <TagName Type="n"> or <TagName>NULL</TagName> *

	skipWhitespace(context);
	matchChar(context, '<');
	matchString(context, pszTagName);
	skipWhitespace(context);

	if (context->input[context->offset] == '>')
	{
		 * Node tag without type property must be NULL. *
		matchString(context, ">NULL");
		*bIsNull = true;
		return 0;
	}

	matchString(context, "type=\"");
	int			offsetEndQuote = findChar(context, '"');

	context->input[offsetEndQuote] = '\0';
	int			type = atoi(context->input + context->offset);

	context->input[offsetEndQuote] = '"';
	bumpOffset(context, offsetEndQuote + 1 - context->offset);

	skipWhitespace(context);
	matchChar(context, '>');

	return type;
}

void
matchNextTag(SerializeContext * context, char *pszTagName)
{
	 * Should be positioned on <TagName>  *

	skipWhitespace(context);
	matchChar(context, '<');
	skipWhitespace(context);
	matchString(context, pszTagName);
	skipWhitespace(context);
	matchChar(context, '>');
}

void
matchNextEndTag(SerializeContext * context, char *pszTagName)
{
	 * Should be positioned on </TagName>  *

	skipWhitespace(context);
	matchString(context, "</");
	skipWhitespace(context);
	matchString(context, pszTagName);
	skipWhitespace(context);
	matchChar(context, '>');
}

bool
checkNextEndTag(SerializeContext * context, char *pszTagName)
{
	bool		rtn = false;
	int			saved_offset = context->offset;

	skipWhitespace(context);

	if (checkString(context, "</"))
	{
		skipWhitespace(context);
		if (checkString(context, pszTagName))
		{
			skipWhitespace(context);
			if (checkChar(context, '>'))
				rtn = true;
		}
	}

	context->offset = saved_offset;

	return rtn;
}

void
skipWhitespace(SerializeContext * context)
{
	while (!context->eof)
	{
		char		c = context->input[context->offset];

		if (c == ' ' || c == '\t' || c == '\n')
			bumpOffset(context, 1);
		else
			break;
	}
}

void
matchChar(SerializeContext * context, char c)
{
	if (context->eof)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("Unexpected eof in serialized string %s.",
						context->input)));
	}

	char		x = context->input[context->offset];

	if (x != c)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("Unexpected character %c found in serialized string %s.  Expecting %c",
						x, context->input, c)));
	}

	bumpOffset(context, 1);
}

bool
checkChar(SerializeContext * context, char c)
{
	if (context->eof)
		return false;

	char		x = context->input[context->offset];

	if (x != c)
		return false;

	bumpOffset(context, 1);

	return true;
}

void
matchString(SerializeContext * context, char *s)
{
	Assert(s != NULL);

	if (context->eof)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("Unexpected eof in serialized string %s.",
						context->input)));
	}

	if (strncmp(context->input + context->offset, s, strlen(s)) != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
		errmsg("Substring %s not found in serialized string %s at offset %d.",
			   s, context->input, context->offset)));
	}

	bumpOffset(context, strlen(s));
}

bool
checkString(SerializeContext * context, char *s)
{
	Assert(s != NULL);

	if (context->eof)
		return false;

	if (strncmp(context->input + context->offset, s, strlen(s)) != 0)
		return false;

	bumpOffset(context, strlen(s));

	return true;
}

void
bumpOffset(SerializeContext * context, int n)
{
	context->offset += n;
	if (context->offset >= strlen(context->input))
		context->eof = true;
}

int
findChar(SerializeContext * context, char c)
{
	char	   *szFound = strchr(context->input + context->offset, c);

	if (szFound == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CDB_INTERNAL_ERROR),
				 errmsg("Expected character %c not found in serialized string %s at offset %d.",
						c, context->input, context->offset)));
	}

	return szFound - context->input;
}

int
getIntValue(SerializeContext * context)
{
	int			nextIndex = findChar(context, '<');

	context->input[nextIndex] = '\0';
	int			val = atoi(context->input + context->offset);

	context->input[nextIndex] = '<';
	bumpOffset(context, nextIndex - context->offset);
	return val;
}

uint
getUIntValue(SerializeContext * context)
{
	int			nextIndex = findChar(context, '<');

	context->input[nextIndex] = '\0';
	uint		val;

	sscanf(context->input + context->offset, "%u", &val);
	context->input[nextIndex] = '<';
	bumpOffset(context, nextIndex - context->offset);
	return val;
}

double
getDoubleValue(SerializeContext * context)
{
	int			nextIndex = findChar(context, '<');

	context->input[nextIndex] = '\0';
	double		val = atof(context->input + context->offset);

	context->input[nextIndex] = '<';
	bumpOffset(context, nextIndex - context->offset);
	return val;
}

bool
getBoolValue(SerializeContext * context)
{
	int			nextIndex = findChar(context, '<');

	context->input[nextIndex] = '\0';
	int			val = atoi(context->input + context->offset);

	context->input[nextIndex] = '<';
	bumpOffset(context, nextIndex - context->offset);

	return val ? true : false;
}

char *
getStringValue(SerializeContext * context)
{
	int			nextIndex = findChar(context, '<');

	context->input[nextIndex] = '\0';
	char	   *val = pstrdup(context->input + context->offset);

	context->input[nextIndex] = '<';
	bumpOffset(context, nextIndex - context->offset);

	return val;
}
*/
#endif

static char *unescape_quotes(const char *src);
static char *escape_quotes(const char *src, int *size);

/*
 * serializeNode -
 * This is used on the query dispatcher to serialize Plan and Query Trees for
 * dispatching to qExecs.
 * The returned string is palloc'ed in the current memory context.
 */
char *
serializeNode(Node *node, int *size)
{
	char	   *pszNode;
	char	   *sNode;

	Assert(node != NULL);

	pszNode = nodeToString(node);
	Assert(pszNode != NULL);

	sNode = escape_quotes(pszNode, size);
	pfree(pszNode);

	return sNode;
}

/*
 * deserializeNode -
 * This is used on the qExecs to deserialize serialized Plan and Query Trees
 * received from the dispatcher.
 * The returned node is palloc'ed in the current memory context.
 */
Node *
deserializeNode(char *strNode)
{
	char		*sNode;
	Node		*node;

	Assert(strNode != NULL);

	sNode = unescape_quotes(strNode);

	Assert(sNode != NULL);

	node = stringToNode(sNode);

	pfree(sNode);

	return node;
}


/*
 * Escape any single quotes in given string
 * 
 * If size argument is non-NULL, store the size (not counting terminating NULL).
 */
static char *
escape_quotes(const char *src, int *size)
{
	int			len = strlen(src),
				i,
				j;
	char	   *result = palloc(len * 2 + 1);

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

/*
 * Unescape any escaped single quotes in given string
 */
static char *
unescape_quotes(const char *src)
{
	int			len = strlen(src),
				i,
				j;
	char	   *result = palloc(len + 1);

	for (i = 0, j = 0; i < len; i++)
	{
		if (i < len - 1 && src[i] == '\\' && (src[i + 1] == '\'' || src[i + 1] == '\\'))
			i++;

		result[j++] = src[i];
	}
	result[j] = '\0';
	return result;
}
