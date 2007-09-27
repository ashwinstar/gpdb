/*--------------------------------------------------------------------------
*
* cdbsreh.c
*	  Provides routines for single row error handling for COPY and external
*	  tables.
*
* Copyright (c) 2007, Greenplum
*
* IDENTIFICATION
*	  $Id$
*
*--------------------------------------------------------------------------
*/
#include "postgres.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/acl.h"
#include "miscadmin.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"
#include "executor/spi.h"

#include "cdb/cdbsreh.h"
#include "cdb/cdbvars.h"

static void OpenErrorTable(CdbSreh *cdbsreh, RangeVar *errortable);
static void InsertIntoErrorTable(CdbSreh *cdbsreh);
static void CloseErrorTable(CdbSreh *cdbsreh);
static int  GetNextSegid(CdbSreh *cdbsreh);
static void VerifyErrorTableAttr(Form_pg_attribute *attr, 
								 int attrnum, 
								 const char *expected_attname,
								 Oid expected_atttype,
								 char *relname);


/*
 * makeCdbSreh
 *
 * Allocate and initialize a Single Row Error Handling state object.
 * Pass in the only known parameters (both we get from the SQL stmt),
 * the other variables are set later on, when they are known.
 */
CdbSreh *
makeCdbSreh(bool should_keep, int rejectlimit, bool is_limit_in_rows, 
			RangeVar *errortable, char *filename, char *relname)
{
	CdbSreh	*h;

	h = palloc(sizeof(CdbSreh));
	
	h->errmsg = NULL;
	h->rawdata = NULL;
	h->linenumber = 0;
	h->relname = relname;
	h->rejectlimit = rejectlimit;
	h->is_limit_in_rows = is_limit_in_rows;
	h->rejectcount = 0;
	h->is_server_enc = false;
	h->should_keep = should_keep;
	h->cdbcopy = NULL;
	h->errtbl = NULL;
	h->lastsegid = 0;
	
	if(filename)
		h->filename = pstrdup(filename);
	else
		h->filename = "<stdin>";
	
	
	/* error table was specified open it (and create it first if necessary) */
	if(errortable)
		OpenErrorTable(h, errortable);
	
	/*
	 * Create a temporary memory context that we can reset once per row to
	 * recover palloc'd memory.  This avoids any problems with leaks inside
	 * datatype input routines, and should be faster than retail pfree's
	 * anyway.
	 */
	h->badrowcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "SrehMemCtxt",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
	
	return h;
}

void
destroyCdbSreh(CdbSreh *cdbsreh)
{
	
	/* delete the bad row context */
    MemoryContextDelete(cdbsreh->badrowcontext);
	
	/* close error table */
	if (cdbsreh->errtbl)
		CloseErrorTable(cdbsreh);
	
	pfree(cdbsreh);
}

/*
 * HandleSingleRowError
 *
 * The single row error handler. Gets called when a data error happened
 * in SREH mode. Reponsible for making a decision of what to do at that time.
 *
 * Some of the main actions are:
 *  - Keep track of reject limit. if reached make sure notify caller.
 *  - If error table used, call the error logger to log this error.
 *  - If QD COPY send the bad row to the QE COPY to deal with.
 *
 */
void HandleSingleRowError(CdbSreh *cdbsreh)
{
	/* 
	 * increment total number of errors for this segment. 
	 *
	 * we increment unless this is the QD and we are using error tables
	 * since in this case the bad row will be sent to a QE and the QE will
	 * be the one to increment the number of errors there. we don't want to 
	 * count the same error twice...
	 */
	if(!(cdbsreh->errtbl && (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT)) )
		cdbsreh->rejectcount++;
	
	/* 
	 * if reached the segment reject limit don't do anything.
	 * (this will get checked and handled later on by the caller).
	 */
	if(cdbsreh->rejectcount >= cdbsreh->rejectlimit)
		return;
	
	/*
	 * If not using an error table, we don't need to do anything.
	 *
	 * However, if we use an error table:
	 * QE - log the error in the error table.
	 * QD - send the bad data row to a random QE (via roundrobin).
	 */
	if(cdbsreh->errtbl)
	{
		if (Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT) 
		{
			cdbCopySendData(cdbsreh->cdbcopy, 
							GetNextSegid(cdbsreh), 
							cdbsreh->rawdata, 
							strlen(cdbsreh->rawdata));
			
		}
		else
		{
			/* Insert into error table */
			Insist(Gp_role == GP_ROLE_EXECUTE || Gp_role == GP_ROLE_UTILITY);
			InsertIntoErrorTable(cdbsreh);
		}
		
	}
	
	return; /* OK */
}

void OpenErrorTable(CdbSreh *cdbsreh, RangeVar *errortable)
{
	AclResult	aclresult;
	AclMode		required_access = ACL_INSERT;

	/* Open and lock the error relation, using the appropriate lock type. */
	cdbsreh->errtbl = heap_openrv(errortable, RowExclusiveLock);
	
	/* Check relation permissions. */
	aclresult = pg_class_aclcheck(RelationGetRelid(cdbsreh->errtbl),
								  GetUserId(),
								  required_access);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(cdbsreh->errtbl));
	
	/* check read-only transaction */
	if (XactReadOnly &&
		!isTempNamespace(RelationGetNamespace(cdbsreh->errtbl)))
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("transaction is read-only")));
	
	/* make sure this is a regular relation */
	if (cdbsreh->errtbl->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" exists in the database but is a non table relation",
						RelationGetRelationName(cdbsreh->errtbl))));

}

void CloseErrorTable(CdbSreh *cdbsreh)
{
	/* close the error relation */
	if (cdbsreh->errtbl)
		heap_close(cdbsreh->errtbl, NoLock);
}


/*
 * InsertIntoErrorTable
 *
 * Insert the information in cdbsreh into the error table we are using.
 */
void InsertIntoErrorTable(CdbSreh *cdbsreh)
{
	HeapTuple	tuple;
	char		nulls[NUM_ERRORTABLE_ATTR];
	Datum		values[NUM_ERRORTABLE_ATTR];
	MemoryContext oldcontext;
					
	oldcontext = MemoryContextSwitchTo(cdbsreh->badrowcontext);
	
	/* Initialize all values for row to NULL */
	MemSet(values, 0, NUM_ERRORTABLE_ATTR * sizeof(Datum));
	MemSet(nulls, 'n', NUM_ERRORTABLE_ATTR * sizeof(char));
	
	/* command start time */
	values[errtable_cmdtime - 1] = TimestampTzGetDatum(GetCurrentStatementStartTimestamp());
	nulls[errtable_cmdtime - 1] = ' ';
		
	/* line number */
	values[errtable_linenum - 1] = Int32GetDatum(cdbsreh->linenumber);
	nulls[errtable_linenum - 1] = ' ';

	if(cdbsreh->is_server_enc)
	{
		/* raw data */
		values[errtable_rawdata - 1] = DirectFunctionCall1(textin, CStringGetDatum(cdbsreh->rawdata));
		nulls[errtable_rawdata - 1] = ' ';		
	}
	else
	{
		/* raw bytes */
		values[errtable_rawbytes - 1] = DirectFunctionCall1(byteain, CStringGetDatum(cdbsreh->rawdata));
		nulls[errtable_rawbytes - 1] = ' ';				
	}
	
	/* file name */
	if(cdbsreh->filename)
	{
		values[errtable_filename - 1] = DirectFunctionCall1(textin, CStringGetDatum(cdbsreh->filename));
		nulls[errtable_filename - 1] = ' ';		
	}

	/* relation name */
	values[errtable_relname - 1] = DirectFunctionCall1(textin, CStringGetDatum(cdbsreh->relname));
	nulls[errtable_relname - 1] = ' ';
	
	/* error message */
	values[errtable_errmsg - 1] = DirectFunctionCall1(textin, CStringGetDatum(cdbsreh->errmsg));
	nulls[errtable_errmsg - 1] = ' ';
	
	
	MemoryContextSwitchTo(oldcontext);
	
	/*
	 * And now we can form the input tuple.
	 */
	tuple = heap_formtuple(RelationGetDescr(cdbsreh->errtbl), values, nulls);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * OK, store the tuple and create index entries for it
	 */
	simple_heap_insert(cdbsreh->errtbl, tuple);
	
	MemoryContextReset(cdbsreh->badrowcontext);
}


/* 
 * ReportSrehResults
 *
 * When necessary emit a NOTICE that describes the end result of the
 * SREH operations. Information includes the total number of rejected
 * rows, and whether rows were ignored or logged into an error table.
 */
void ReportSrehResults(CdbSreh *cdbsreh, int total_rejected)
{
	if(total_rejected > 0)
	{
		if(cdbsreh && cdbsreh->errtbl)
			ereport(NOTICE, 
					(errmsg("Rejected %d badly formatted rows. Errors logged into "
							"error table \"%s\"", 
							total_rejected, 
							RelationGetRelationName(cdbsreh->errtbl))));
		else
			ereport(NOTICE, 
					(errmsg("Rejected %d badly formatted rows.", 
							total_rejected)));
	}
}


/*
 * SendNumRowsRejected
 *
 * Using this function the QE sends back to the client QD the number 
 * of rows that were rejected in this last data load in SREH mode.
 */
void SendNumRowsRejected(int numrejected)
{
	StringInfoData buf;
	
	Insist(Gp_role == GP_ROLE_EXECUTE);
	pq_beginmessage(&buf, 'j'); /* 'j' is the msg code for rejected records */
	pq_sendint(&buf, numrejected, 4);
	pq_endmessage(&buf);	
}

/*
 * ValidateErrorTableMetaData
 *
 * This function gets called if a user wants an already existing table to be
 * used as an error table for some COPY or external table operation with SREH.
 * In here we verify that the metadata of the user selected table matches the
 * predefined requirement for an error table.
 */
void ValidateErrorTableMetaData(Relation rel)
{
	TupleDesc   tupDesc = RelationGetDescr(rel);
	Form_pg_attribute *attr = tupDesc->attrs;
	TupleConstr *constr = tupDesc->constr;
	char		*relname = RelationGetRelationName(rel);
	int			attr_count = tupDesc->natts;
	
	/*
	 * Verify number of attributes match
	 */
	if(attr_count != NUM_ERRORTABLE_ATTR)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Relation \"%s\" already exists and is not of a valid "
						"error table format (expected %d attributes, found %d)",
						relname, NUM_ERRORTABLE_ATTR, attr_count)));
	
	/*
	 * Verify this table has no constraints
	 *
	 * (TODO: this currently checks for all the contraints in TupleConstr
	 *        which are, defaults, check, and not null. We need to check 
	 *		  for unique constraints as well).
	 */
	if(constr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Relation \"%s\" already exists and is not of a valid "
						"error table format. If appears to have constraints "
						"defined.", relname)));
	
	/*
	 * TODO: verify it's distributed randomly.
	 */
	
	/*
	 * run through each attribute at a time and verify it's what we expect
	 */
	VerifyErrorTableAttr(attr, errtable_cmdtime, "cmdtime", TIMESTAMPTZOID, relname);
	VerifyErrorTableAttr(attr, errtable_relname, "relname", TEXTOID, relname);
	VerifyErrorTableAttr(attr, errtable_filename, "filename", TEXTOID, relname);
	VerifyErrorTableAttr(attr, errtable_linenum, "linenum", INT4OID, relname);
	VerifyErrorTableAttr(attr, errtable_bytenum, "bytenum", INT4OID, relname);
	VerifyErrorTableAttr(attr, errtable_errmsg, "errmsg", TEXTOID, relname);
	VerifyErrorTableAttr(attr, errtable_rawdata, "rawdata", TEXTOID, relname);
	VerifyErrorTableAttr(attr, errtable_rawbytes, "rawbytes", BYTEAOID, relname);
	
}

/*
 * VerifyErrorTableAttr
 *
 * Called by ValidateErrorTableMetaData() on each table attribute to verify
 * that it has the right predefined name, type and other attr characteristics.
 */
void VerifyErrorTableAttr(Form_pg_attribute *attr, 
						  int attrnum, 
						  const char *expected_attname,
						  Oid expected_atttype,
						  char *relname)
{
	
	bool		cur_attisdropped = attr[attrnum - 1]->attisdropped;
	Name		cur_attname = &(attr[attrnum - 1]->attname);
	Oid			cur_atttype = attr[attrnum - 1]->atttypid;
	int4		cur_attndims = attr[attrnum - 1]->attndims;
		
	if(cur_attisdropped)
		(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
		 errmsg("Relation \"%s\" includes dropped attributes and is "
				"therefore not of a valid error table format",
				relname));

	if (namestrcmp(cur_attname, expected_attname) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Relation \"%s\" is an invalid error table. Expected "
						"attribute \"%s\" found \"%s\"",
						relname, expected_attname, NameStr(*cur_attname))));
	
	if(cur_atttype != expected_atttype)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Relation \"%s\" is an invalid error table. Wrong data "
						"type for attribute \"%s\"",
						relname, NameStr(*cur_attname))));
	
	if(cur_attndims > 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("Relation \"%s\" is an invalid error table. Attribute "
						"\"%s\" has more than zero dimensions (array).",
						relname, NameStr(*cur_attname))));
	
	
}
/*
 * GetNextSegid
 *
 * Return the next sequential segment id of available segids (roundrobin).
 */
static
int GetNextSegid(CdbSreh *cdbsreh)
{
	int total_segs = cdbsreh->cdbcopy->total_segs;
	
	if(cdbsreh->lastsegid == total_segs)
		cdbsreh->lastsegid = 0; /* start over from first segid */
	
	return (cdbsreh->lastsegid++ % total_segs);
}


