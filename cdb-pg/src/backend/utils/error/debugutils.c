/* 
 * debugutils.c
 * 
 * Routines for debugging Greenplum DB
 * 
 * Copyright (c) 2007, Greenplum
 */

#include "postgres.h"
#include "fmgr.h"

#include <stdio.h>
#include <unistd.h>

#include "nodes/plannodes.h"
#include "utils/debugutils.h"
#include "utils/lsyscache.h"


static void
printatt(unsigned attributeId,
		 Form_pg_attribute attributeP,
		 char *value, char *buf)
{
	sprintf(buf,
			 "\t%2d: %s%s%s%s\t(typeid = %u, len = %d, typmod = %d, byval = %c)|",
		   attributeId,
		   NameStr(attributeP->attname),
		   value != NULL ? " = \"" : "",
		   value != NULL ? value : "",
		   value != NULL ? "\"" : "",
		   (unsigned int) (attributeP->atttypid),
		   attributeP->attlen,
		   attributeP->atttypmod,
		   attributeP->attbyval ? 't' : 'f');
}

/*
 * Return a human readable string representation of a tuple. The returned
 * string must be pfree()d by the caller.
 */

char * 
tup2str(TupleTableSlot *slot)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	int			natts = typeinfo->natts;
	int			i;
	Datum		origattr,
				attr;
	char	   *value;
	bool		isnull;
	Oid			typoutput;
	bool		typisvarlena;
	char	   *buf;

	buf = palloc(10 * 1024);
	buf[0] = '\0';

	for (i = 0; i < natts; ++i)
	{
		origattr = slot_getattr(slot, i + 1, &isnull);
		if (isnull)
			continue;
		getTypeOutputInfo(typeinfo->attrs[i]->atttypid,
						  &typoutput, &typisvarlena);

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (typisvarlena)
			attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
		else
			attr = origattr;

		value = OidOutputFunctionCall(typoutput, attr);

		printatt((unsigned) i + 1, typeinfo->attrs[i], value, 
				 &buf[strlen(buf)]);

		pfree(value);

		/* Clean up detoasted copy, if any */
		if (attr != origattr)
			pfree(DatumGetPointer(attr));
	}
	return buf;
}

extern char *plannode_type(Plan *p);

static void
drawnode(FILE *ofile, Plan *plan, List *nodelist)
{
	if(!plan)
		return;

	/* print out the node */
	fprintf(ofile, "\"Node_0x%p\" [\n", plan);
	fprintf(ofile, "label=\"%s %p flow %p\"\n", plannode_type(plan), plan, get_plan_flow(plan));
	fprintf(ofile, "];\n");

	if(IsA(plan, Append))
	{
		Append *app = (Append *) plan;
		ListCell *cell;
		foreach(cell, app->appendplans)
		{
			Plan *child = (Plan *) lfirst(cell);
			if(!list_member_ptr(nodelist, child))
			{
				nodelist = lappend(nodelist, child);
				drawnode(ofile, child, nodelist);
			}

			fprintf(ofile, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
			fprintf(ofile, "];\n");
		}
	}
	else if (IsA(plan, SubqueryScan))
	{
		SubqueryScan *subq = (SubqueryScan *) plan;
		Plan *child = subq->subplan;
		if(!list_member_ptr(nodelist, child))
		{
			nodelist = lappend(nodelist, child);
			drawnode(ofile, child, nodelist);
		}

		fprintf(ofile, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
		fprintf(ofile, "];\n");
	}
	else
	{
		Plan *child = plan->lefttree;
		if(child)
		{
			if(!list_member_ptr(nodelist, child))
			{
				nodelist = lappend(nodelist, child);
				drawnode(ofile, child, nodelist);
			}

			fprintf(ofile, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
			fprintf(ofile, "];\n");
		}
		child = plan->righttree;
		if(child)
		{
			if(!list_member_ptr(nodelist, child))
			{
				nodelist = lappend(nodelist, child);
				drawnode(ofile, child, nodelist);
			}

			fprintf(ofile, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
			fprintf(ofile, "];\n");
		}
	}
}

/*
 * Write out a plan to be interpretted by dot(1). Tools available at
 * http://www.graphviz.org
 */
void
dotnode(void *node, const char *fname)
{
	List *nodelist = NULL;
	FILE *ofile = fopen(fname, "w+");

	/* Print dot header */

	fprintf(ofile, "digraph g {\n");
	fprintf(ofile, "graph [\n");
	fprintf(ofile, "];\n");

	fprintf(ofile, "node [\n");
	fprintf(ofile, "fontsize = \"14\"\n");
	fprintf(ofile, "shape = \"box\"\n");
	fprintf(ofile, "];\n");
	fprintf(ofile, "edge [\n");
	fprintf(ofile, "fontsize = \"14\"\n");
	fprintf(ofile, "];\n");

	if(node)
	{
		nodelist = lappend(nodelist, node);
		drawnode(ofile, (Plan *) node, nodelist);
	}

	fprintf(ofile, "}\n");
	fclose(ofile);
}
