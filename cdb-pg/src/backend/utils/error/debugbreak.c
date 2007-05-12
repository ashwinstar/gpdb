/* 
 * debugbreak.c
 * 	Debugging facilities
 * Copyright (c) 2007, greenplum inc
 */

#include <unistd.h>
#include "postgres.h"
#include "utils/debugbreak.h"

#include "lib/stringinfo.h"
#include "nodes/plannodes.h"

/* How to use debug_break
 *
 * Greenplum DB will spawn many processes to execute a query in parallel.
 * In order to debug, one may need to first run a query that will successfully
 * create enough process (slice), attach gdb to these slices, and set break points
 * in all GDB sessions.  This is quite tedious.
 *
 * Alternatively, you can put a sleep somewhere in the code, so the process running
 * the interesting code will sleep and you will have enough time to attach gdb.
 * The problem is that it is a tedious process to "unfreeze" the sleeped process,
 * and, you cannot disable the "sleep point".
 *
 * Here is the more convinient debugging facilities.
 * Suppose you want to inspecting some code in slice 3 (from logging, or crash, etc).
 * At the point you want to break in, add the follwing
 * 		if(currentSliceId == 3) 
 * 			debug_break();
 * Run the query, slice 3 will sleep here.  You can find the process id from 
 * log and attach debugger.  In order to "unfreeze" the process, you go to the frame
 * of debug_break, and in gdb, do,
 * 		gdb> p debug_break_flag = 0
 *		gdb> c
 * this will unfreeze the process.  If you just want the process keep running and 
 * do not hit any debug_break any more, do
 * 		gdb> p debug_break_flag = 2
 *		gdb> c
 * Now debug_break will not hit again.
 *
 * Two very good place to put debug break are,
 * 		1. ExceptionalCondition, to catch assert.
 *		2. errfinish, before PG_RE_THROW, to catch exceptions
 */

/*
 * XXX: NOTE: all the operations really should be "interlocked" ops, but
 * so far I did not run into any problem (postgres is not threaded)
 */
#ifdef USE_DEBUG_BREAK

void debug_break()
{
	static volatile int debug_break_loop = 0; 

	++debug_break_loop; 

	if(debug_break_loop == 1)
		elog(LOG, "Debug break");

	while(debug_break_loop == 1)
		sleep(1);
}

/* debug_break_n
 * debug_break will turn on/off all debug_breaks, the _n version will turn on/off
 * a debug_break point controlled by number n.
 */

#define DEBUG_BREAK_N_MAX (sizeof(int) * 8)

/* by default, only enable debug_break_point 1, (Assert) */
static volatile int debug_break_n_flags = 1; 

void debug_break_n(int n)
{
	int checkbit = 1;
	static volatile int debug_break_n_loop = 0;

	Assert(n >= 1 && n<= DEBUG_BREAK_N_MAX);
	
	checkbit <<= (n-1);
	++debug_break_n_loop;

	if((debug_break_n_flags & checkbit) && debug_break_n_loop == 1)
		elog(LOG, "Debug break n: %d", n);

	while((debug_break_n_flags & checkbit) && debug_break_n_loop == 1)
		sleep(1);
}

void enable_debug_break_n(int n)
{
	if(n == 0)
		debug_break_n_flags = ~0;
	else
	{
		int bit = 1;
		Assert(n >= 1 && n <= DEBUG_BREAK_N_MAX);
		bit <<= (n-1);
		debug_break_n_flags |= bit;
	}
}

void disable_debug_break_n(int n)
{
	if(n == 0)
		debug_break_n_flags = 0;
	else
	{
		int bit = 1;
		Assert(n >= 1 && n <= DEBUG_BREAK_N_MAX);
		bit <<= (n-1);
		debug_break_n_flags &= ~bit;
	}
}

#endif

extern char *plannode_type(Plan *p);

static void drawnode(StringInfo str, Plan *plan, List *nodelist)
{
	if(!plan)
		return;

	/* print out the node */
	appendStringInfo(str, "\"Node_0x%p\" [\n", plan);
	appendStringInfo(str, "label=\"%s 0x%p\"\n", plannode_type(plan), plan);
	appendStringInfo(str, "];\n");

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
				drawnode(str, child, nodelist);
			}

			appendStringInfo(str, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
			appendStringInfo(str, "];\n");
		}
	}
	else if (IsA(plan, SubqueryScan))
	{
		SubqueryScan *subq = (SubqueryScan *) plan;
		Plan *child = subq->subplan;
		if(!list_member_ptr(nodelist, child))
		{
			nodelist = lappend(nodelist, child);
			drawnode(str, child, nodelist);
		}

		appendStringInfo(str, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
		appendStringInfo(str, "];\n");
	}
	else
	{
		Plan *child = plan->lefttree;
		if(child)
		{
			if(!list_member_ptr(nodelist, child))
			{
				nodelist = lappend(nodelist, child);
				drawnode(str, child, nodelist);
			}

			appendStringInfo(str, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
			appendStringInfo(str, "];\n");
		}
		child = plan->righttree;
		if(child)
		{
			if(!list_member_ptr(nodelist, child))
			{
				nodelist = lappend(nodelist, child);
				drawnode(str, child, nodelist);
			}

			appendStringInfo(str, "\"Node_0x%p\" -> \"Node_0x%p\" [\n", plan, child);
			appendStringInfo(str, "];\n");
		}
	}
}

char* dotnode(void * node)
{
	Plan *plan = (Plan *) node;
	StringInfo str = makeStringInfo(); 
	List *nodelist = NULL;

	/* Print dot header */
	appendStringInfo(str, "digraph g {\n");
	appendStringInfo(str, "graph [\n");
	appendStringInfo(str, "];\n");

	appendStringInfo(str, "node [\n");
	appendStringInfo(str, "fontsize = \"14\"\n");
	appendStringInfo(str, "shape = \"box\"\n");
	appendStringInfo(str, "];\n");
	appendStringInfo(str, "edge [\n");
	appendStringInfo(str, "fontsize = \"14\"\n");
	appendStringInfo(str, "];\n");

	if(node)
	{
		nodelist = lappend(nodelist, node);
		drawnode(str, (Plan *) node, nodelist);
	}

	appendStringInfo(str, "}\n");
	return str->data;
}


	
