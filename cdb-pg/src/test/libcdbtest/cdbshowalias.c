/*-------------------------------------------------------------------------
 *
 * cdbshowalias.c
 *
 */
#include "postgres.h"

#include <ctype.h>
#include <assert.h>
#include "tcop/tcopprot.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "nodes/pg_list.h"

#include "cdbshowalias.h"

/* general utility */
#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

/*
 * cdb_show_alias takes 1 argument:
 *  1) an SQL statement
 * It returns a string which is an alias definition for the result set of the SQL statement.
 */
PG_FUNCTION_INFO_V1(cdb_show_alias);
Datum
cdb_show_alias(PG_FUNCTION_ARGS)
{
	char			*pszSQL = GET_STR(PG_GETARG_TEXT_P(0));
	int				i;
	StringInfoData	buffer;


	initStringInfo( &buffer );

	/* Get tuple desc that will be the result of executing this
	 * sql statement 
	 * Step 1: create a parse tree from the sql statement.
	 *			Validate that there is only 1.
	 * Step 2: create a query tree from the parse tree.
	 *			Validate that it is a select statement.
	 * Step 3: get the type for each of the result columns.
	 */
    List	*raw_parsetree_list;

	/*
	 * Here we get the parse tree for the sql statement
	 */
	raw_parsetree_list = pg_parse_query(pszSQL);
	
	/*
	 *  Validate that there was only 1 statement
	 */
	if ( list_length(raw_parsetree_list) != 1 )
	{
        ereport(ERROR,
                (errcode(ERRCODE_MOST_SPECIFIC_TYPE_MISMATCH),
                    errmsg("Function cdb_show_alias() cannot process multiple statements."))
                );
	}

	/*
	 *  This gets the query tree from the parse tree.
	 */
	Node *parsetree = (Node *)linitial(raw_parsetree_list);
	List *query_list = pg_analyze_and_rewrite( parsetree, NULL, 0);
	
	/*
	 * Validate that there is only one query tree
	 */
	if ( list_length(query_list) != 1 )
	{
        ereport(ERROR,
                (errcode(ERRCODE_MOST_SPECIFIC_TYPE_MISMATCH),
                    errmsg("Function cdb_show_alias() cannot process multiple statements."))
                );
	}
		
	Query *queryTree = (Query *)linitial(query_list);
	
	// Make sure that this is a select statement
	if ( queryTree->type != T_Query || 
		queryTree->commandType != CMD_SELECT )
	{
        ereport(ERROR,
                (errcode(ERRCODE_MOST_SPECIFIC_TYPE_MISMATCH),
                    errmsg("Function cdb_show_alias() can only process SELECT statements."))
                );
	}
	
	List *targetList = queryTree->targetList;

	i = 0;

	appendStringInfo(&buffer, "as T(");

	
	/*
	 * Follow the linked list of TargetEntry's from the query tree's targetList
	 * Then check the resdom, and if it's a true result column,
	 * get the typeoid and typemod
	 * and compare these to the same fields from the attrs array
	 */
	ListCell *lc;
	foreach( lc, targetList )
	{
		TargetEntry	*target;
		Resdom		*tr;

		target = (TargetEntry *)(lfirst(lc));
		tr = target->resdom;

		if ( !tr->resjunk )
		{
			if ( i > 0 )
				appendStringInfo(&buffer, ", ");

			appendStringInfo(&buffer, "%s %s", 
				tr->resname, 
				format_type_be(tr->restype));
			
			i++;
		}
	}
	
	appendStringInfo(&buffer, ")");

	int lenOutput = strlen(buffer.data);
	int len = lenOutput + VARHDRSZ;
	text* result = (text *) palloc(len);

	/* Set size of result string... */
	VARATT_SIZEP(result) = len;

	/* Fill data field of result string... */
	char* ptr = VARDATA(result);
	memcpy(ptr, buffer.data, lenOutput);

	pfree(pszSQL);
	pfree(buffer.data);

	PG_RETURN_TEXT_P(result);
}

