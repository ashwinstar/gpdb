/*-------------------------------------------------------------------------
 *
 * spi.c
 *				Server Programming Interface
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/spi.c,v 1.165.2.2 2006/12/26 16:56:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "catalog/heap.h"
#include "commands/trigger.h"
#include "executor/spi_priv.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

#include "libpq-fe.h"
#include "libpq/libpq-be.h"
#include "libpq-int.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbdisp.h"
#include "miscadmin.h"
#include "commands/dbcommands.h"	/* get_database_name() */
#include "postmaster/postmaster.h"  /* PostPortNumber */

#include "nodes/print.h"

uint32		SPI_processed = 0;
Oid			SPI_lastoid = InvalidOid;
SPITupleTable *SPI_tuptable = NULL;
int			SPI_result;

static _SPI_connection *_SPI_stack = NULL;
static _SPI_connection *_SPI_current = NULL;
static int	_SPI_stack_depth = 0;		/* allocated size of _SPI_stack */
static int	_SPI_connected = -1;
static int	_SPI_curid = -1;

static PGconn * _QD_conn = NULL; 		/* To call back to the QD for SQL execution */
static 	char	   *_QD_currently_prepared_stmt = NULL;

static void _SPI_prepare_plan(const char *src, _SPI_plan *plan);

static int _SPI_execute_plan(_SPI_plan *plan,
				  Datum *Values, const char *Nulls,
				  Snapshot snapshot, Snapshot crosscheck_snapshot,
				  bool read_only, long tcount);

static int	_SPI_pquery(QueryDesc *queryDesc, long tcount);

static void _SPI_error_callback(void *arg);

static void _SPI_cursor_operation(Portal portal, bool forward, long count,
					  DestReceiver *dest);

static _SPI_plan *_SPI_copy_plan(_SPI_plan *plan, int location);

static int	_SPI_begin_call(bool execmem);
static int	_SPI_end_call(bool procmem);
static MemoryContext _SPI_execmem(void);
static MemoryContext _SPI_procmem(void);
static bool _SPI_checktuples(void);

static int connect_back_to_qd(void);

static bool can_execute_this_locally(const char * sql);

bool gp_use_snapshop_during_callback = true;

/* used only in testing in test/libcdbtest ?? */
extern int
			SPI_execute_cdb(const char *src, Plan *pPlan, Query *pQuery, bool read_only, int tcount);


bool
can_execute_this_locally(const char * src)
{
	List * raw_parsetree_list;
	ListCell * list_item;
	bool	can_run_local = true;
	bool	is_a_select = true;
	
	/*
	 * Utility mode never uses (or needs) the special callback code
	 */
	if (Mpp_role == MPP_ROLE_UTILITY)
		return true;
	/*
	 * If we are running on the QD, we should be able to execute any function without problem,
	 * because the QD knows how to dispatch SQL statements if necessary.
	 */
	if (Mpp_role == MPP_ROLE_DISPATCH)
		return true;
		
	/*
	 * Parse the request string into a list of raw parse trees.
	 */
	raw_parsetree_list = pg_parse_query(src);
	
	/*
	 * If we have only SELECT statements with no FROM clauses, we should
	 * be able to execute them locally, even on the QE.  Most often, this is something 
	 * like   SELECT $1
	 * Functions use that type of SELECT to evaluate expressions, so without those,
	 * no functions would be useful.
	 * 
	 * We also need to execute certain catalog queries locally.  The Fault-Tolerance system
	 * does queries of gp_configuration, and some DDL and Utility commands do selects from the 
	 * catalog table, etc.    So, if the FROM clause consists only of catalog tables, we 
	 * will run the query locally.
	 * 
	 */
	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);
		if (IsA(parsetree,SelectStmt))
		{
			SelectStmt * s = (SelectStmt *)parsetree;
			if (s->into != NULL || s->op != SETOP_NONE)
			{
				can_run_local = false;
				break;
			}
				
			if (s->fromClause != NULL)
			{
				ListCell * f;
				foreach(f,s->fromClause)
				{
					Node * fr = (Node *) lfirst(f);
					RangeVar * r;
					if (!IsA(fr,RangeVar))
					{
						can_run_local = false;
						break;
					}
					r = (RangeVar *)fr;
					if (r->schemaname && strcmp(r->schemaname,"pg_catalog")!=0)
						can_run_local = false;
					if (r->schemaname==NULL && r->relname && strncmp(r->relname,"pg_",3)!=0)
						can_run_local = false;
						
					if (!can_run_local)
						break;
				}
				
			}
		}
		else
		{
			can_run_local = false;
			is_a_select = false;
		}
			
		if (!can_run_local)
			break;

	}
	
	elog(DEBUG1,"SPI running SQL, local=%d:  %s",can_run_local, src);
	
	
	if (can_run_local) 
		return true;

	/*
	 * Ok, we have an SQL statement, that probably can only be dispatched by the QD
	 */
	 
	if (!gp_enable_functions)
		elog(ERROR, "Functions that execute SQL statements from the segDBs are not yet supported");
		
	if (Mpp_role == MPP_ROLE_EXECUTE && !Gp_is_primary && !is_a_select)
		elog(ERROR, "Functions that execute non-SELECT SQL statements from the mirrorDBs are not supported");

	return false;
}
	
/* =================== interface functions =================== */

int
SPI_connect(void)
{
	int			newdepth;

	/*
	 * When procedure called by Executor _SPI_curid expected to be equal to
	 * _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected)
		return SPI_ERROR_CONNECT;

	if (_SPI_stack == NULL)
	{
		if (_SPI_connected != -1 || _SPI_stack_depth != 0)
			elog(ERROR, "SPI stack corrupted");
		newdepth = 16;
		_SPI_stack = (_SPI_connection *)
			MemoryContextAlloc(TopTransactionContext,
							   newdepth * sizeof(_SPI_connection));
		_SPI_stack_depth = newdepth;
	}
	else
	{
		if (_SPI_stack_depth <= 0 || _SPI_stack_depth <= _SPI_connected)
			elog(ERROR, "SPI stack corrupted");
		if (_SPI_stack_depth == _SPI_connected + 1)
		{
			newdepth = _SPI_stack_depth * 2;
			_SPI_stack = (_SPI_connection *)
				repalloc(_SPI_stack,
						 newdepth * sizeof(_SPI_connection));
			_SPI_stack_depth = newdepth;
		}
	}

	/*
	 * We're entering procedure where _SPI_curid == _SPI_connected - 1
	 */
	_SPI_connected++;
	Assert(_SPI_connected >= 0 && _SPI_connected < _SPI_stack_depth);

	_SPI_current = &(_SPI_stack[_SPI_connected]);
	_SPI_current->processed = 0;
	_SPI_current->lastoid = InvalidOid;
	_SPI_current->tuptable = NULL;
	_SPI_current->procCxt = NULL;		/* in case we fail to create 'em */
	_SPI_current->execCxt = NULL;
	_SPI_current->connectSubid = GetCurrentSubTransactionId();

	/*
	 * Create memory contexts for this procedure
	 *
	 * XXX it would be better to use PortalContext as the parent context, but
	 * we may not be inside a portal (consider deferred-trigger execution).
	 * Perhaps CurTransactionContext would do?	For now it doesn't matter
	 * because we clean up explicitly in AtEOSubXact_SPI().
	 */
	_SPI_current->procCxt = AllocSetContextCreate(TopTransactionContext,
												  "SPI Proc",
												  ALLOCSET_DEFAULT_MINSIZE,
												  ALLOCSET_DEFAULT_INITSIZE,
												  ALLOCSET_DEFAULT_MAXSIZE);
	_SPI_current->execCxt = AllocSetContextCreate(TopTransactionContext,
												  "SPI Exec",
												  ALLOCSET_DEFAULT_MINSIZE,
												  ALLOCSET_DEFAULT_INITSIZE,
												  ALLOCSET_DEFAULT_MAXSIZE);
	/* ... and switch to procedure's context */
	_SPI_current->savedcxt = MemoryContextSwitchTo(_SPI_current->procCxt);
	
	if (_QD_conn)
		elog(DEBUG1,"SPI_connect found that we were already connected back to the QD");

	return SPI_OK_CONNECT;
}


/*
 * Note that we cannot free any connection back to the QD at SPI_finish time.
 * Our transaction may not be complete yet, so we don't yet know if the work
 * done on the QD should be committed or rolled back.
 */
int
SPI_finish(void)
{
	int			res;

	res = _SPI_begin_call(false);		/* live in procedure memory */
	if (res < 0)
		return res;

	/* Restore memory context as it was before procedure call */
	MemoryContextSwitchTo(_SPI_current->savedcxt);

	/* Release memory used in procedure call */
	MemoryContextDelete(_SPI_current->execCxt);
	_SPI_current->execCxt = NULL;
	MemoryContextDelete(_SPI_current->procCxt);
	_SPI_current->procCxt = NULL;

	/*
	 * Reset result variables, especially SPI_tuptable which is probably
	 * pointing at a just-deleted tuptable
	 */
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;

	/*
	 * After _SPI_begin_call _SPI_connected == _SPI_curid. Now we are closing
	 * connection to SPI and returning to upper Executor and so _SPI_connected
	 * must be equal to _SPI_curid.
	 */
	_SPI_connected--;
	_SPI_curid--;
	if (_SPI_connected == -1)
		_SPI_current = NULL;
	else
		_SPI_current = &(_SPI_stack[_SPI_connected]);

	return SPI_OK_FINISH;
}

/* for callback to QD */
static void
build_mppqdid_params(StringInfoData * buf)
{
	appendStringInfo(buf, "mppqdid=");

	appendStringInfo(buf, "%d;", gp_session_id);

#ifdef HAVE_INT64_TIMESTAMP
	appendStringInfo(buf, INT64_FORMAT ";", PgStartTime);
#else
	appendStringInfo(buf, "%.14a;", PgStartTime);
#endif

	/* change last semicolon to space */
	Assert(buf->data[buf->len - 1] == ';');
	buf->data[buf->len - 1] = ' ';
}	/* build_mppqdid_params */



int connect_back_to_qd(void)
{
	 
	StringInfoData buffer;
	
	/* Do we already have a connection back to the QD ? */	
	if (_QD_conn && PQstatus(_QD_conn) == CONNECTION_OK)
		return SPI_OK_CONNECT;
		
	/* Never do this in utility mode */
	if (Mpp_role == MPP_ROLE_UTILITY)
		return SPI_OK_CONNECT;
		
	_QD_currently_prepared_stmt = NULL;
	
	initStringInfo(&buffer);
		
		
	build_mppqdid_params(&buffer);
		
	/*
	 * Setting options is mostly just to help with debugging.
	 */
	appendStringInfo(&buffer, "options='");

	appendStringInfo(&buffer, " -c gp_qd_proc_offset=%ld",(long)gp_qd_proc_offset);
	
	switch (client_min_messages)
	{
		case DEBUG5:
				appendStringInfo(&buffer, " -c client_min_messages=DEBUG5");
			break;
		case DEBUG4:
			appendStringInfo(&buffer, " -c client_min_messages=DEBUG4");
			break;
		case DEBUG3:
			appendStringInfo(&buffer, " -c client_min_messages=DEBUG3");
			break;
		case DEBUG2:
			appendStringInfo(&buffer, " -c client_min_messages=DEBUG2");
			break;
		case DEBUG1:
			appendStringInfo(&buffer, " -c client_min_messages=DEBUG1");
			break;
		case LOG:
			appendStringInfo(&buffer, " -c client_min_messages=LOG");
			break;
		case INFO:
			appendStringInfo(&buffer, " -c client_min_messages=INFO");
			break;
		case NOTICE:
			appendStringInfo(&buffer, " -c client_min_messages=NOTICE");
			break;
		case WARNING:
			appendStringInfo(&buffer, " -c client_min_messages=WARNING");
			break;
		case ERROR:
			appendStringInfo(&buffer, " -c client_min_messages=ERROR");
			break;
		case FATAL:
			appendStringInfo(&buffer, " -c client_min_messages=FATAL");
			break;
		case PANIC:
			appendStringInfo(&buffer, " -c client_min_messages=PANIC");
			break;
		default:;
	}



	if (superuser())
	{
		/* some super-user settable gucs */
		switch (log_min_messages)
		{
			case DEBUG5:
				appendStringInfo(&buffer, " -c log_min_messages=DEBUG5");
				break;
			case DEBUG4:
				appendStringInfo(&buffer, " -c log_min_messages=DEBUG4");
				break;
			case DEBUG3:
				appendStringInfo(&buffer, " -c log_min_messages=DEBUG3");
				break;
			case DEBUG2:
				appendStringInfo(&buffer, " -c log_min_messages=DEBUG2");
				break;
			case DEBUG1:
				appendStringInfo(&buffer, " -c log_min_messages=DEBUG1");
				break;
			case LOG:
				appendStringInfo(&buffer, " -c log_min_messages=LOG");
				break;
			case INFO:
				appendStringInfo(&buffer, " -c log_min_messages=INFO");
				break;

			case WARNING:
				appendStringInfo(&buffer, " -c log_min_messages=WARNING");
				break;
			case ERROR:
				appendStringInfo(&buffer, " -c log_min_messages=ERROR");
				break;
			case FATAL:
				appendStringInfo(&buffer, " -c log_min_messages=FATAL");
				break;
			case PANIC:
				appendStringInfo(&buffer, " -c log_min_messages=PANIC");
				break;
			default:;
		}

		if (Log_error_verbosity != PGERROR_DEFAULT)
		{
			if (Log_error_verbosity == PGERROR_VERBOSE)
				appendStringInfo(&buffer, " -c log_error_verbosity=verbose");
			if (Log_error_verbosity == PGERROR_TERSE)
				appendStringInfo(&buffer, " -c log_error_verbosity=terse");
		}

		switch (log_min_error_statement)
		{
			case DEBUG5:
				appendStringInfo(&buffer, " -c log_min_error_statement=DEBUG5");
				break;
			case DEBUG4:
				appendStringInfo(&buffer, " -c log_min_error_statement=DEBUG4");
				break;
			case DEBUG3:
				appendStringInfo(&buffer, " -c log_min_error_statement=DEBUG3");
				break;
			case DEBUG2:
				appendStringInfo(&buffer, " -c log_min_error_statement=DEBUG2");
				break;
			case DEBUG1:
				appendStringInfo(&buffer, " -c log_min_error_statement=DEBUG1");
				break;
			case LOG:
				appendStringInfo(&buffer, " -c log_min_error_statement=LOG");
				break;
			case INFO:
				appendStringInfo(&buffer, " -c log_min_error_statement=INFO");
				break;
			case NOTICE:
				appendStringInfo(&buffer, " -c log_min_error_statement=NOTICE");
				break;
			case WARNING:
				appendStringInfo(&buffer, " -c log_min_error_statement=WARNING");
				break;
			case ERROR:
				appendStringInfo(&buffer, " -c log_min_error_statement=ERROR");
				break;
			case FATAL:
				appendStringInfo(&buffer, " -c log_min_error_statement=FATAL");
				break;

			default:;
		}

	}
	appendStringInfo(&buffer, "' ");
	/* End of options */
	
	/* Set up connection back to the QD */
	
	/* 
	 * The QD might be multi-homed (more than one IP address), and only one of the IP
	 * addresses might work for us.  So, we don't rely on the QD sending us an IP,
	 * instead we look to see the remote IP address of our session.  This should always
	 * be one that works.
	 */
	if (MyProcPort->remote_host && strcmp(MyProcPort->remote_host,"[local]")!=0)
	{
		appendStringInfo(&buffer,"host=");
		appendStringInfo(&buffer,MyProcPort->remote_host);
		appendStringInfo(&buffer," ");
	}
	
	appendStringInfo(&buffer,"dbname=");
	if (MyProcPort->database_name)
		appendStringInfo(&buffer,MyProcPort->database_name);
	else
		appendStringInfo(&buffer,get_database_name(MyDatabaseId));
		
	appendStringInfo(&buffer," user=");
	if (MyProcPort->user_name)
		appendStringInfo(&buffer,MyProcPort->user_name);
	else
		appendStringInfo(&buffer,GetUserNameFromId(GetUserId()));
		
	appendStringInfo(&buffer," ");
	/*
	 * The one this we can't guess is the port, so the QD passes that to us
	 */
	if (Mpp_role == MPP_ROLE_DISPATCH)
		appendStringInfo(&buffer,"port=%d",PostPortNumber);
	else
		appendStringInfo(&buffer,gp_qd_callback_info);
 
	 
	
 	elog(DEBUG1,"Connecting to QD: %s",buffer.data);
 	
	/* Make a connection to the database */
    _QD_conn = PQconnectdb(buffer.data);
    
     /* Check to see that the backend connection was successfully made */
    if (PQstatus(_QD_conn) != CONNECTION_OK)
    {
    	
    	if (Mpp_role == MPP_ROLE_EXECUTE)
    	{
    		elog(ERROR, "Connection Failed: %s", PQerrorMessage(_QD_conn));
    	}
    	else
    	{
    		elog(NOTICE,"Connection Failed: %s",PQerrorMessage(_QD_conn));
    	}
    	PQfinish(_QD_conn);
    	_QD_conn = NULL;
    	return SPI_ERROR_CONNECT;
    }
    elog(DEBUG1,"Connection succeeded");	
	return SPI_OK_CONNECT;
}

/*
 * Clean up SPI state at transaction commit or abort.
 */
void
AtEOXact_SPI(bool isCommit)
{
	/*
	 * Note that memory contexts belonging to SPI stack entries will be freed
	 * automatically, so we can ignore them here.  We just need to restore our
	 * static variables to initial state.
	 */
	if (isCommit && _SPI_connected != -1)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("transaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));

	if (_QD_conn)
	{
		/* 
		 * If we are connected back to the QD, and we hit end-of-transaction, we
		 * need to tell the QD to end that transaction as well.
		 * 
		 * We need to make sure it commits if we commit, and it rolls back if we 
		 * rollback.   It would be even better if we involved it in the 2pc.
		 */
		PGresult * res = 0;
		/*elog(DEBUG1,"atEOXact_SPI %d",isCommit);
		
		elog(DEBUG1,"Transaction status %d",PQtransactionStatus(_QD_conn));*/
		
		if (!isCommit)
			res = PQexec(_QD_conn,"ROLLBACK");
		else
			res = PQexec(_QD_conn,"COMMIT");

		if (PQresultStatus(res) != PGRES_COMMAND_OK)
	    {
	        elog(NOTICE, PQerrorMessage(_QD_conn));
	    }
	 	PQclear(res);
		
		/*
		 * Now that we are done, let's disconnect from the QD.  It may make
		 * more sense to try to save the connection and avoid the overhead of 
		 * reconnecting again in the future, but it seems more important to free
		 * up the QD connection slot as soon as possible, as that is a very
		 * limited resource. 
		 */

		/* disconnection from the database */
	    PQfinish(_QD_conn);
	    
	    _QD_conn = NULL;
	}

	_SPI_current = _SPI_stack = NULL;
	_SPI_stack_depth = 0;
	_SPI_connected = _SPI_curid = -1;
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;
}

/*
 * Clean up SPI state at subtransaction commit or abort.
 *
 * During commit, there shouldn't be any unclosed entries remaining from
 * the current subtransaction; we emit a warning if any are found.
 */
void
AtEOSubXact_SPI(bool isCommit, SubTransactionId mySubid)
{
	bool		found = false;
	
	if (_QD_conn)
		elog(DEBUG1,"atEOSubXact_SPI %d",isCommit);

	while (_SPI_connected >= 0)
	{
		_SPI_connection *connection = &(_SPI_stack[_SPI_connected]);

		if (connection->connectSubid != mySubid)
			break;				/* couldn't be any underneath it either */

		found = true;

		/*
		 * Release procedure memory explicitly (see note in SPI_connect)
		 */
		if (connection->execCxt)
		{
			MemoryContextDelete(connection->execCxt);
			connection->execCxt = NULL;
		}
		if (connection->procCxt)
		{
			MemoryContextDelete(connection->procCxt);
			connection->procCxt = NULL;
		}

		/*
		 * Pop the stack entry and reset global variables.	Unlike
		 * SPI_finish(), we don't risk switching to memory contexts that might
		 * be already gone.
		 */
		_SPI_connected--;
		_SPI_curid = _SPI_connected;
		if (_SPI_connected == -1)
			_SPI_current = NULL;
		else
			_SPI_current = &(_SPI_stack[_SPI_connected]);
		SPI_processed = 0;
		SPI_lastoid = InvalidOid;
		SPI_tuptable = NULL;
	}

	if (found && isCommit)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("subtransaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));

	/*
	 * If we are aborting a subtransaction and there is an open SPI context
	 * surrounding the subxact, clean up to prevent memory leakage.
	 */
	if (_SPI_current && !isCommit)
	{
		/* free Executor memory the same as _SPI_end_call would do */
		MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
		/* throw away any partially created tuple-table */
		SPI_freetuptable(_SPI_current->tuptable);
		_SPI_current->tuptable = NULL;
	}
}


/* Pushes SPI stack to allow recursive SPI calls */
void
SPI_push(void)
{
	_SPI_curid++;
}

/* Pops SPI stack to allow recursive SPI calls */
void
SPI_pop(void)
{
	_SPI_curid--;
}

/* Restore state of SPI stack after aborting a subtransaction */
void
SPI_restore_connection(void)
{
	Assert(_SPI_connected >= 0);
	if (_QD_conn)
		elog(DEBUG1,"SPI_restore_connection");
	_SPI_curid = _SPI_connected - 1;
}

/* Parse, plan, and execute a query string */
int
SPI_execute(const char *src, bool read_only, long tcount)
{
	_SPI_plan	plan;
	int			res;

	if (src == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	plan.plancxt = NULL;		/* doesn't have own context */
	plan.query = src;
	plan.nargs = 0;
	plan.argtypes = NULL;

	_SPI_prepare_plan(src, &plan);

	res = _SPI_execute_plan(&plan, NULL, NULL,
							InvalidSnapshot, InvalidSnapshot,
							read_only, tcount);

	_SPI_end_call(true);
	return res;
}

/* Obsolete version of SPI_execute */
int
SPI_exec(const char *src, long tcount)
{
	return SPI_execute(src, false, tcount);
}

/* Execute a previously prepared plan */
int
SPI_execute_plan(void *plan, Datum *Values, const char *Nulls,
				 bool read_only, long tcount)
{
	int			res;

	if (plan == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (((_SPI_plan *) plan)->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan((_SPI_plan *) plan,
							Values, Nulls,
							InvalidSnapshot, InvalidSnapshot,
							read_only, tcount);

	_SPI_end_call(true);
	return res;
}

/* Obsolete version of SPI_execute_plan */
int
SPI_execp(void *plan, Datum *Values, const char *Nulls, long tcount)
{
	return SPI_execute_plan(plan, Values, Nulls, false, tcount);
}

/*
 * SPI_execute_snapshot -- identical to SPI_execute_plan, except that we allow
 * the caller to specify exactly which snapshots to use.  This is currently
 * not documented in spi.sgml because it is only intended for use by RI
 * triggers.
 *
 * Passing snapshot == InvalidSnapshot will select the normal behavior of
 * fetching a new snapshot for each query.
 */
int
SPI_execute_snapshot(void *plan,
					 Datum *Values, const char *Nulls,
					 Snapshot snapshot, Snapshot crosscheck_snapshot,
					 bool read_only, long tcount)
{
	int			res;

	if (plan == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (((_SPI_plan *) plan)->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan((_SPI_plan *) plan,
							Values, Nulls,
							snapshot, crosscheck_snapshot,
							read_only, tcount);

	_SPI_end_call(true);
	return res;
}

void *
SPI_prepare(const char *src, int nargs, Oid *argtypes)
{
	_SPI_plan	plan;
	_SPI_plan  *result;

	if (src == NULL || nargs < 0 || (nargs > 0 && argtypes == NULL))
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	plan.plancxt = NULL;		/* doesn't have own context */
	plan.query = src;
	plan.nargs = nargs;
	plan.argtypes = argtypes;

	_SPI_prepare_plan(src, &plan);

	/* copy plan to procedure context */
	result = _SPI_copy_plan(&plan, _SPI_CPLAN_PROCXT);

	_SPI_end_call(true);

	return (void *) result;
}

void *
SPI_saveplan(void *plan)
{
	_SPI_plan  *newplan;

	if (plan == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(false);		/* don't change context */
	if (SPI_result < 0)
		return NULL;

	newplan = _SPI_copy_plan((_SPI_plan *) plan, _SPI_CPLAN_TOPCXT);

	_SPI_curid--;
	SPI_result = 0;

	return (void *) newplan;
}

int
SPI_freeplan(void *plan)
{
	_SPI_plan  *spiplan = (_SPI_plan *) plan;

	if (plan == NULL)
		return SPI_ERROR_ARGUMENT;

	MemoryContextDelete(spiplan->plancxt);
	return 0;
}

HeapTuple
SPI_copytuple(HeapTuple tuple)
{
	MemoryContext oldcxt = NULL;
	HeapTuple	ctuple;

	if (tuple == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}

	ctuple = heap_copytuple(tuple);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return ctuple;
}

HeapTupleHeader
SPI_returntuple(HeapTuple tuple, TupleDesc tupdesc)
{
	MemoryContext oldcxt = NULL;
	HeapTupleHeader dtup;

	if (tuple == NULL || tupdesc == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	/* For RECORD results, make sure a typmod has been assigned */
	if (tupdesc->tdtypeid == RECORDOID &&
		tupdesc->tdtypmod < 0)
		assign_record_type_typmod(tupdesc);

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}

	dtup = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy((char *) dtup, (char *) tuple->t_data, tuple->t_len);

	HeapTupleHeaderSetDatumLength(dtup, tuple->t_len);
	HeapTupleHeaderSetTypeId(dtup, tupdesc->tdtypeid);
	HeapTupleHeaderSetTypMod(dtup, tupdesc->tdtypmod);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return dtup;
}

HeapTuple
SPI_modifytuple(Relation rel, HeapTuple tuple, int natts, int *attnum,
				Datum *Values, const char *Nulls)
{
	MemoryContext oldcxt = NULL;
	HeapTuple	mtuple;
	int			numberOfAttributes;
	Datum	   *v;
	char	   *n;
	int			i;

	if (rel == NULL || tuple == NULL || natts < 0 || attnum == NULL || Values == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}
	SPI_result = 0;
	numberOfAttributes = rel->rd_att->natts;
	v = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	n = (char *) palloc(numberOfAttributes * sizeof(char));

	/* fetch old values and nulls */
	heap_deformtuple(tuple, rel->rd_att, v, n);

	/* replace values and nulls */
	for (i = 0; i < natts; i++)
	{
		if (attnum[i] <= 0 || attnum[i] > numberOfAttributes)
			break;
		v[attnum[i] - 1] = Values[i];
		n[attnum[i] - 1] = (Nulls && Nulls[i] == 'n') ? 'n' : ' ';
	}

	if (i == natts)				/* no errors in *attnum */
	{
		mtuple = heap_formtuple(rel->rd_att, v, n);

		/*
		 * copy the identification info of the old tuple: t_ctid, t_self, and
		 * OID (if any)
		 */
		mtuple->t_data->t_ctid = tuple->t_data->t_ctid;
		mtuple->t_self = tuple->t_self;
		mtuple->t_tableOid = tuple->t_tableOid;
		if (rel->rd_att->tdhasoid)
			HeapTupleSetOid(mtuple, HeapTupleGetOid(tuple));
	}
	else
	{
		mtuple = NULL;
		SPI_result = SPI_ERROR_NOATTRIBUTE;
	}

	pfree(v);
	pfree(n);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return mtuple;
}

int
SPI_fnumber(TupleDesc tupdesc, const char *fname)
{
	int			res;
	Form_pg_attribute sysatt;

	for (res = 0; res < tupdesc->natts; res++)
	{
		if (namestrcmp(&tupdesc->attrs[res]->attname, fname) == 0)
			return res + 1;
	}

	sysatt = SystemAttributeByName(fname, true /* "oid" will be accepted */ );
	if (sysatt != NULL)
		return sysatt->attnum;

	/* SPI_ERROR_NOATTRIBUTE is different from all sys column numbers */
	return SPI_ERROR_NOATTRIBUTE;
}

char *
SPI_fname(TupleDesc tupdesc, int fnumber)
{
	Form_pg_attribute att;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		att = tupdesc->attrs[fnumber - 1];
	else
		att = SystemAttributeDefinition(fnumber, true);

	return pstrdup(NameStr(att->attname));
}

char *
SPI_getvalue(HeapTuple tuple, TupleDesc tupdesc, int fnumber)
{
	char	   *result;
	Datum		origval,
				val;
	bool		isnull;
	Oid			typoid,
				foutoid;
	bool		typisvarlena;

	SPI_result = 0;

	if (fnumber > HeapTupleHeaderGetNatts(tuple->t_data) || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	origval = heap_getattr(tuple, fnumber, tupdesc, &isnull);
	if (isnull)
		return NULL;

	if (fnumber > 0)
		typoid = tupdesc->attrs[fnumber - 1]->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber, true))->atttypid;

	getTypeOutputInfo(typoid, &foutoid, &typisvarlena);

	/*
	 * If we have a toasted datum, forcibly detoast it here to avoid memory
	 * leakage inside the type's output routine.
	 */
	if (typisvarlena)
		val = PointerGetDatum(PG_DETOAST_DATUM(origval));
	else
		val = origval;

	result = OidOutputFunctionCall(foutoid, val);

	/* Clean up detoasted copy, if any */
	if (val != origval)
		pfree(DatumGetPointer(val));

	return result;
}

Datum
SPI_getbinval(HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull)
{
	SPI_result = 0;

	if (fnumber > HeapTupleHeaderGetNatts(tuple->t_data) || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		*isnull = true;
		return (Datum) NULL;
	}

	return heap_getattr(tuple, fnumber, tupdesc, isnull);
}

char *
SPI_gettype(TupleDesc tupdesc, int fnumber)
{
	Oid			typoid;
	HeapTuple	typeTuple;
	char	   *result;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		typoid = tupdesc->attrs[fnumber - 1]->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber, true))->atttypid;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(typoid),
							   0, 0, 0);

	if (!HeapTupleIsValid(typeTuple))
	{
		SPI_result = SPI_ERROR_TYPUNKNOWN;
		return NULL;
	}

	result = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(typeTuple))->typname));
	ReleaseSysCache(typeTuple);
	return result;
}

Oid
SPI_gettypeid(TupleDesc tupdesc, int fnumber)
{
	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return InvalidOid;
	}

	if (fnumber > 0)
		return tupdesc->attrs[fnumber - 1]->atttypid;
	else
		return (SystemAttributeDefinition(fnumber, true))->atttypid;
}

char *
SPI_getrelname(Relation rel)
{
	return pstrdup(RelationGetRelationName(rel));
}

char *
SPI_getnspname(Relation rel)
{
	return get_namespace_name(RelationGetNamespace(rel));
}

void *
SPI_palloc(Size size)
{
	MemoryContext oldcxt = NULL;
	void	   *pointer;

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}

	pointer = palloc(size);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return pointer;
}

void *
SPI_repalloc(void *pointer, Size size)
{
	/* No longer need to worry which context chunk was in... */
	return repalloc(pointer, size);
}

void
SPI_pfree(void *pointer)
{
	/* No longer need to worry which context chunk was in... */
	pfree(pointer);
}

void
SPI_freetuple(HeapTuple tuple)
{
	/* No longer need to worry which context tuple was in... */
	heap_freetuple(tuple);
}

void
SPI_freetuptable(SPITupleTable *tuptable)
{
	if (tuptable != NULL)
		MemoryContextDelete(tuptable->tuptabcxt);
}



/*
 * SPI_cursor_open()
 *
 *	Open a prepared SPI plan as a portal
 */
Portal
SPI_cursor_open(const char *name, void *plan,
				Datum *Values, const char *Nulls,
				bool read_only)
{
	_SPI_plan  *spiplan = (_SPI_plan *) plan;
	List	   *qtlist;
	List	   *ptlist;
	ParamListInfo paramLI;
	Snapshot	snapshot;
	MemoryContext oldcontext;
	Portal		portal;
	int			k;
	
	/* 
	 * If we can't execute this SELECT locally, make a connection back to the QD
	 * so the QD can execute it.
	 */
	 
	if (spiplan->run_via_callback_to_qd)
	{
		PGresult *res;
		const char * *paramValues = NULL;
		int *paramLengths = NULL;
		int *paramFormats = NULL;
		int			nargs = spiplan->nargs;
		int i;
		const char * cursorname;
		char * query;

		portal = NULL;
		
		connect_back_to_qd();
		
		if (_QD_conn==NULL)
		{
			elog(ERROR,"This SQL statement can not yet be executed in this stored procedure");
		}
		
		if (name == NULL || name[0] == '\0')
			cursorname="mycursor";
		else
			cursorname = name;
		query = palloc(strlen(spiplan->query)+strlen(cursorname)+40);
		sprintf(query, "declare %s cursor for %s",cursorname,spiplan->query);
		elog(NOTICE,"SPI_cursor_open via QD: %s",query);
		
		res = PQexec(_QD_conn, "BEGIN");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
	    {
	        elog(ERROR, PQerrorMessage(_QD_conn)); 
	    }
	    PQclear(res);
		
		//if (_QD_currently_prepared_stmt == NULL ||	
		//	strcmp(_QD_currently_prepared_stmt, query) !=0)
		{
			
			elog(DEBUG1,"Re-preparing plan: %s",query);
			res = PQprepare(_QD_conn,
                "",
                query,
                spiplan->nargs,
                spiplan->argtypes);
                    
	        if (PQresultStatus(res) != PGRES_COMMAND_OK)
		    {
		        elog(ERROR, PQerrorMessage(_QD_conn));
		        PQclear(res);
		        
		    }
		    _QD_currently_prepared_stmt = pstrdup(query);
		}
		
		/* Reset SPI result (note we deliberately don't touch lastoid) */
		SPI_processed = 0;
		SPI_tuptable = NULL;
		_SPI_current->processed = 0;
		_SPI_current->lastoid = InvalidOid;
		_SPI_current->tuptable = NULL;
		
		
		
		if (nargs > 0)
		{
			paramValues = palloc(nargs*sizeof(char*));
			paramLengths = palloc(nargs*sizeof(int));
			paramFormats = palloc(nargs*sizeof(int));

			for (i=0;i<nargs;i++)
			{
				char	   *outputbytes=0;
				Oid			typsend;
				bool		typisvarlena;
				FmgrInfo finfo;
				Datum val;
				
				if (Nulls && Nulls[i] == 'n')
				{
					outputbytes = "";
				}
				else
				{
		
					/* Get functionId of output function */
					getTypeOutputInfo(spiplan->argtypes[i], &typsend, &typisvarlena);
					fmgr_info(typsend, &finfo);
					 
									/* Call the attribute type's output converter. */
					if (finfo.fn_nargs != 1)
						elog(WARNING,"Function wants more than one arg");
						
					if (typisvarlena)
						val = PointerGetDatum(PG_DETOAST_DATUM(Values[i]));
					else
						val = Values[i];
						
					outputbytes = DatumGetCString(FunctionCall1(&finfo, val));
				}
																		 
				paramValues[i] = outputbytes;
				paramLengths[i] = strlen(outputbytes);
				paramFormats[i] = 0;   // 0 = text, 1 = binary
				
				
			}
		}
                    
        res = PQexecPrepared(_QD_conn,
                         "",
                         nargs,  
                         paramValues,
                         paramLengths,
                         paramFormats,
                         0 /*resultFormat*/);   
                    
        if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
	    {
	    	PQclear(res);
	        elog(ERROR, PQerrorMessage(_QD_conn));
	    }

        if (PQresultStatus(res) == PGRES_TUPLES_OK)
        {
        	int i;
        	int j;
        	HeapTuple tuple;
        	Datum *values;
			char *nulls;
			TupleDesc tempTupleDesc;
			TupleTableSlot slot;
			Oid			typrecv;
			Oid			typioparam;
			FmgrInfo finfo;
			elog(DEBUG1,"PGRES_TUPLES_OK");
        	
        	_SPI_current->processed = PQntuples(res);
        	
        	tempTupleDesc = CreateTemplateTupleDesc(PQnfields(res), false);
        	
        	for(i=0;i<PQnfields(res);i++)
        		TupleDescInitEntry(tempTupleDesc, (AttrNumber) i+1, PQfname(res,i), PQftype(res,i), PQfmod(res,i), 0 /*attdim*/);
        
        	if (tempTupleDesc->tdtypeid == RECORDOID && tempTupleDesc->tdtypmod < 0)
				assign_record_type_typmod(tempTupleDesc);
				
        	/* This sets up tuptable */
        	spi_dest_startup(NULL, 0, tempTupleDesc);
        	
        	
			nulls = palloc(PQnfields(res)+1);
			values = palloc(PQnfields(res)*sizeof(Datum));
        	
        	for(i=0;i<PQntuples(res);i++)
        	{
        		for(j=0;j<PQnfields(res);j++)
        		{
        			char * v;
        			nulls[j] = ' ';
        			if (PQgetisnull(res,i,j))
        			{
        				nulls[j] = 'n';
        				values[j] = 0;
        			}
        			else
        			{
	        			v = PQgetvalue(res,i,j);
	        			
						/* Get functionId of output function */
						getTypeInputInfo(tempTupleDesc->attrs[j]->atttypid, &typrecv, &typioparam);
						fmgr_info(typrecv, &finfo);
	        			
						if (finfo.fn_nargs == 1)
							values[j] = FunctionCall1(&finfo,
										  CStringGetDatum(v));
					    else if (finfo.fn_nargs == 2)
					    	values[j] = FunctionCall2(&finfo,
										  CStringGetDatum(v),
									   ObjectIdGetDatum(typioparam));
						else
							values[j] = FunctionCall3(&finfo,
										  CStringGetDatum(v),
									   ObjectIdGetDatum(typioparam),
									Int32GetDatum(-1));
        			}
        			 
        		}
        		tuple = heap_formtuple(tempTupleDesc, values, nulls);
				//if (objectid != (Oid) 0)
					//HeapTupleSetOid(tuple, objectid);
				memset(&slot,0,sizeof(TupleTableSlot));
				slot.tts_tuple = tuple;
				slot.tts_tupleDescriptor = tempTupleDesc;
				slot.tts_isempty = false;
				spi_printtup(&slot, NULL);
				
        	}
        			
        	SPI_processed = _SPI_current->processed;
			SPI_lastoid = _SPI_current->lastoid;
			SPI_tuptable = _SPI_current->tuptable;
	        
	        
        	 
        } 
        
        return portal;
	}
	
	elog(DEBUG1,"SPI_cursor_open local: %s",spiplan->query);
	

	/*
	 * Check that the plan is something the Portal code will special-case as
	 * returning one tupleset.
	 */
	if (!SPI_is_cursor_plan(spiplan))
	{
		/* try to give a good error message */
		Query	   *queryTree;

		if (list_length(spiplan->qtlist) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
					 errmsg("cannot open multi-query plan as cursor")));
		queryTree = PortalListGetPrimaryQuery((List *) linitial(spiplan->qtlist));
		if (queryTree == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
					 errmsg("cannot open empty query as cursor")));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* translator: %s is name of a SQL command, eg INSERT */
				 errmsg("cannot open %s query as cursor",
						CreateQueryTag(queryTree))));
	}

	Assert(list_length(spiplan->qtlist) == 1);
	qtlist = (List *) linitial(spiplan->qtlist);
	ptlist = spiplan->ptlist;
	if (list_length(qtlist) != list_length(ptlist))
		elog(ERROR, "corrupted SPI plan lists");

	/* Reset SPI result (note we deliberately don't touch lastoid) */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* Create the portal */
	if (name == NULL || name[0] == '\0')
	{
		/* Use a random nonconflicting name */
		portal = CreateNewPortal();
	}
	else
	{
		/* In this path, error if portal of same name already exists */
		portal = CreatePortal(name, false, false);
	}

	/* Switch to portal's memory and copy the parsetrees and plans to there */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
	qtlist = copyObject(qtlist);
	ptlist = copyObject(ptlist);

	/* If the plan has parameters, set them up */
	if (spiplan->nargs > 0)
	{
		/* sizeof(ParamListInfoData) includes the first array element */
		paramLI = (ParamListInfo) palloc(sizeof(ParamListInfoData) +
							  (spiplan->nargs - 1) *sizeof(ParamExternData));
		paramLI->numParams = spiplan->nargs;

		for (k = 0; k < spiplan->nargs; k++)
		{
			ParamExternData *prm = &paramLI->params[k];

			prm->ptype = spiplan->argtypes[k];
			prm->pflags = 0;
			prm->isnull = (Nulls && Nulls[k] == 'n');
			if (prm->isnull)
			{
				/* nulls just copy */
				prm->value = Values[k];
			}
			else
			{
				/* pass-by-ref values must be copied into portal context */
				int16		paramTypLen;
				bool		paramTypByVal;

				get_typlenbyval(prm->ptype, &paramTypLen, &paramTypByVal);
				prm->value = datumCopy(Values[k],
									   paramTypByVal, paramTypLen);
			}
		}
	}
	else
		paramLI = NULL;

	/*
	 * Set up the portal.
	 */
	PortalDefineQuery(portal,
					  NULL,		/* no statement name */
					  spiplan->query,
					  T_SelectStmt,
					  CreateQueryTag(PortalListGetPrimaryQuery(qtlist)),
					  qtlist,
					  ptlist,
					  PortalGetHeapMemory(portal));

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Set up options for portal.
	 */
	portal->cursorOptions &= ~(CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL);
	if (list_length(ptlist) == 1 &&
		ExecSupportsBackwardScan((Plan *) linitial(ptlist)))
		portal->cursorOptions |= CURSOR_OPT_SCROLL;
	else
		portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;

	/*
	 * MPP needs this
	 */
	portal->is_cursor = true;
	
	/*
	 * Set up the snapshot to use.	(PortalStart will do CopySnapshot, so we
	 * skip that here.)
	 */
	if (read_only)
		snapshot = ActiveSnapshot;
	else
	{
		CommandCounterIncrement();
		snapshot = GetTransactionSnapshot();
	}

	/*
	 * Before starting this portal, we need to make sure that the shared
	 * snapshot is already set by a writer gang, or the cursor query readers
	 * will timeout waiting for one that may not exist (in some cases).
	 * Therefore we insert a small hack here and dispatch a SET query that
	 * will do it for us. This is also done in the bind-execute protocol.
	 */
	verify_shared_snapshot_ready();
	
	/*
	 * Start portal execution.
	 */
	PortalStart(portal, paramLI, snapshot);

	Assert(portal->strategy != PORTAL_MULTI_QUERY);

	/* Return the created portal */
	return portal;
}


/*
 * SPI_cursor_find()
 *
 *	Find the portal of an existing open cursor
 */
Portal
SPI_cursor_find(const char *name)
{
	elog(DEBUG1,"SPI_cursor_find");
	return GetPortalByName(name);
}


/*
 * SPI_cursor_fetch()
 *
 *	Fetch rows in a cursor
 */
void
SPI_cursor_fetch(Portal portal, bool forward, long count)
{
	elog(DEBUG1,"SPI_cursor_fetch");
	_SPI_cursor_operation(portal, forward, count,
						  CreateDestReceiver(DestSPI, NULL));
	/* we know that the DestSPI receiver doesn't need a destroy call */
}


/*
 * SPI_cursor_move()
 *
 *	Move in a cursor
 */
void
SPI_cursor_move(Portal portal, bool forward, long count)
{
	elog(DEBUG1,"SPI_cursor_move");
	_SPI_cursor_operation(portal, forward, count, None_Receiver);
}


/*
 * SPI_cursor_close()
 *
 *	Close a cursor
 */
void
SPI_cursor_close(Portal portal)
{
	elog(DEBUG1,"SPI_cursor_close");
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	PortalDrop(portal, false);
}

/*
 * Returns the Oid representing the type id for argument at argIndex. First
 * parameter is at index zero.
 */
Oid
SPI_getargtypeid(void *plan, int argIndex)
{
	if (plan == NULL || argIndex < 0 || argIndex >= ((_SPI_plan *) plan)->nargs)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return InvalidOid;
	}
	return ((_SPI_plan *) plan)->argtypes[argIndex];
}

/*
 * Returns the number of arguments for the prepared plan.
 */
int
SPI_getargcount(void *plan)
{
	if (plan == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return -1;
	}
	return ((_SPI_plan *) plan)->nargs;
}

/*
 * Returns true if the plan contains exactly one command
 * and that command returns tuples to the caller (eg, SELECT or
 * INSERT ... RETURNING, but not SELECT ... INTO). In essence,
 * the result indicates if the command can be used with SPI_cursor_open
 *
 * Parameters
 *	  plan: A plan previously prepared using SPI_prepare
 */
bool
SPI_is_cursor_plan(void *plan)
{
	_SPI_plan  *spiplan = (_SPI_plan *) plan;

	if (spiplan == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return false;
	}

	if (list_length(spiplan->qtlist) != 1)
		return false;			/* not exactly 1 pre-rewrite command */

	switch (ChoosePortalStrategy((List *) linitial(spiplan->qtlist)))
	{
		case PORTAL_ONE_SELECT:
		case PORTAL_ONE_RETURNING:
		case PORTAL_UTIL_SELECT:
			/* OK */
			return true;

		case PORTAL_MULTI_QUERY:
			/* will not return tuples */
			break;
	}
	return false;
}

/*
 * SPI_result_code_string --- convert any SPI return code to a string
 *
 * This is often useful in error messages.	Most callers will probably
 * only pass negative (error-case) codes, but for generality we recognize
 * the success codes too.
 */
const char *
SPI_result_code_string(int code)
{
	static char buf[64];

	switch (code)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
			return "SPI_ERROR_UNCONNECTED";
		case SPI_ERROR_CURSOR:
			return "SPI_ERROR_CURSOR";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_TRANSACTION:
			return "SPI_ERROR_TRANSACTION";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		case SPI_OK_CONNECT:
			return "SPI_OK_CONNECT";
		case SPI_OK_FINISH:
			return "SPI_OK_FINISH";
		case SPI_OK_FETCH:
			return "SPI_OK_FETCH";
		case SPI_OK_UTILITY:
			return "SPI_OK_UTILITY";
		case SPI_OK_SELECT:
			return "SPI_OK_SELECT";
		case SPI_OK_SELINTO:
			return "SPI_OK_SELINTO";
		case SPI_OK_INSERT:
			return "SPI_OK_INSERT";
		case SPI_OK_DELETE:
			return "SPI_OK_DELETE";
		case SPI_OK_UPDATE:
			return "SPI_OK_UPDATE";
		case SPI_OK_CURSOR:
			return "SPI_OK_CURSOR";
		case SPI_OK_INSERT_RETURNING:
			return "SPI_OK_INSERT_RETURNING";
		case SPI_OK_DELETE_RETURNING:
			return "SPI_OK_DELETE_RETURNING";
		case SPI_OK_UPDATE_RETURNING:
			return "SPI_OK_UPDATE_RETURNING";
	}
	/* Unrecognized code ... return something useful ... */
	sprintf(buf, "Unrecognized SPI code %d", code);
	return buf;
}

/* =================== private functions =================== */

/*
 * spi_dest_startup
 *		Initialize to receive tuples from Executor into SPITupleTable
 *		of current SPI procedure
 */
void
spi_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;
	MemoryContext tuptabcxt;

	/*
	 * When called by Executor _SPI_curid expected to be equal to
	 * _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected || _SPI_connected < 0)
		elog(ERROR, "improper call to spi_dest_startup");
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(ERROR, "SPI stack corrupted");

	if (_SPI_current->tuptable != NULL)
		elog(ERROR, "improper call to spi_dest_startup");

	oldcxt = _SPI_procmem();	/* switch to procedure memory context */

	tuptabcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "SPI TupTable",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(tuptabcxt);

	_SPI_current->tuptable = tuptable = (SPITupleTable *)
		palloc(sizeof(SPITupleTable));
	tuptable->tuptabcxt = tuptabcxt;
	tuptable->alloced = tuptable->free = 128;
	tuptable->vals = (HeapTuple *) palloc(tuptable->alloced * sizeof(HeapTuple));
	tuptable->tupdesc = CreateTupleDescCopy(typeinfo);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * spi_printtup
 *		store tuple retrieved by Executor into SPITupleTable
 *		of current SPI procedure
 */
void
spi_printtup(TupleTableSlot *slot, DestReceiver *self)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;

	/*
	 * When called by Executor _SPI_curid expected to be equal to
	 * _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected || _SPI_connected < 0)
		elog(ERROR, "improper call to spi_printtup");
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(ERROR, "SPI stack corrupted");

	tuptable = _SPI_current->tuptable;
	if (tuptable == NULL)
		elog(ERROR, "improper call to spi_printtup");

	oldcxt = MemoryContextSwitchTo(tuptable->tuptabcxt);

	if (tuptable->free == 0)
	{
		tuptable->free = 256;
		tuptable->alloced += tuptable->free;
		tuptable->vals = (HeapTuple *) repalloc(tuptable->vals,
									  tuptable->alloced * sizeof(HeapTuple));
	}

	tuptable->vals[tuptable->alloced - tuptable->free] =
		ExecCopySlotTuple(slot);
	(tuptable->free)--;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Static functions
 */

/*
 * Parse and plan a querystring.
 *
 * At entry, plan->argtypes and plan->nargs must be valid.
 *
 * Query and plan lists are stored into *plan.
 */
static void
_SPI_prepare_plan(const char *src, _SPI_plan *plan)
{
	List	   *raw_parsetree_list;
	List	   *query_list_list;
	List	   *plan_list;
	ListCell   *list_item;
	ErrorContextCallback spierrcontext;
	Oid		   *argtypes = plan->argtypes;
	int			nargs = plan->nargs;
	
	_QD_currently_prepared_stmt = NULL;
	
	/*
	 * Increment CommandCounter to see changes made by now.  We must do this
	 * to be sure of seeing any schema changes made by a just-preceding SPI
	 * command.  (But we don't bother advancing the snapshot, since the
	 * planner generally operates under SnapshotNow rules anyway.)
	 */
	CommandCounterIncrement();

	/*
	 * Setup error traceback support for ereport()
	 */
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = (void *) src;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;
	
	/* 
	 * If we can't execute this prepare locally, connect back to the QD and
	 * ask it to do the prepare.
	 */
	if (can_execute_this_locally(src))
	{
		plan->run_via_callback_to_qd = false;
	}
	else
	{
		plan->run_via_callback_to_qd = true;
		
		connect_back_to_qd();
	}
	
	if (_QD_conn && plan->run_via_callback_to_qd)
	{
		PGresult * res;
		plan->qtlist = NULL;
		plan->ptlist = NULL;
		plan->query = src;
		
		elog(DEBUG1,"_SPI_prepare_plan:  %s",src);
		
		/*
		 * Ok, this is kind of a mess. _SPI_prepare_plan really wants to return an actual
		 * execution plan tree back to the caller.  But, I don't know how we could do that
		 * via libpq calls to the QD, and we can't run the planner locally on the QEs.
		 * 
		 * So, the best we can do is to do a normal prepare call.  We do it as an unnamed
		 * prepared statement, since we will never know the right time to release the prepared 
		 * statement by name.
		 * 
		 */
		res = PQprepare(_QD_conn,
                    "",
                    src,
                    nargs,
                    argtypes);
                    
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
	    {
	        elog(ERROR, PQerrorMessage(_QD_conn));
	        PQclear(res);
	        return;
	    }
		
		_QD_currently_prepared_stmt = pstrdup(src);
        
        /*
		 * Pop the error context stack
		 */
		error_context_stack = spierrcontext.previous;
        
        return;
	}
	

	/*
	 * Parse the request string into a list of raw parse trees.
	 */
	raw_parsetree_list = pg_parse_query(src);

	/*
	 * Do parse analysis and rule rewrite for each raw parsetree.
	 *
	 * We save the querytrees from each raw parsetree as a separate sublist.
	 * This allows _SPI_execute_plan() to know where the boundaries between
	 * original queries fall.
	 */
	query_list_list = NIL;
	plan_list = NIL;

	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);
		List	   *query_list;

		query_list = pg_analyze_and_rewrite(parsetree, src, argtypes, nargs);

		query_list_list = lappend(query_list_list, query_list);

		plan_list = list_concat(plan_list,
								pg_plan_queries(query_list, NULL, false));
	}

	plan->qtlist = query_list_list;
	plan->ptlist = plan_list;
	
	elog_node_display(DEBUG5,"_SPI_prepare_plan queryTree",plan->qtlist,true);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = spierrcontext.previous;
}

/*
 * Execute the given plan with the given parameter values
 *
 * snapshot: query snapshot to use, or InvalidSnapshot for the normal
 *		behavior of taking a new snapshot for each query.
 * crosscheck_snapshot: for RI use, all others pass InvalidSnapshot
 * read_only: TRUE for read-only execution (no CommandCounterIncrement)
 * tcount: execution tuple-count limit, or 0 for none
 */
static int
_SPI_execute_plan(_SPI_plan *plan, Datum *Values, const char *Nulls,
				  Snapshot snapshot, Snapshot crosscheck_snapshot,
				  bool read_only, long tcount)
{
	volatile int my_res = 0;
	volatile uint32 my_processed = 0;
	volatile Oid my_lastoid = InvalidOid;
	SPITupleTable *volatile my_tuptable = NULL;
	volatile int res = 0;
	Snapshot	saveActiveSnapshot;

	/* Be sure to restore ActiveSnapshot on error exit */
	saveActiveSnapshot = ActiveSnapshot;
	
	/* 
	 * If we can't execute this SQL statement locally, let's connect back to the QD
	 * and ask it to execute the command for us.
	 */

		
	
	if (plan->run_via_callback_to_qd)
	{
		PGresult *pgres = NULL;
		const char * *paramValues = NULL;
		int *paramLengths = NULL;
		int *paramFormats = NULL;
		int			nargs = plan->nargs;
		int i;
		int txnOptions = 0;
			
		
		elog(DEBUG1,"_SPI_execute_plan via QD: %s",plan->query);
		if (Mpp_role == MPP_ROLE_EXECUTE)
		{
			if (!Gp_is_primary)
				elog(DEBUG1,"Stored procedure running on mirror...");
			if (!Gp_is_writer)
				elog(DEBUG1,"Stored procedure running in reader gang.");
		}
		
		connect_back_to_qd();
		
		if (_QD_conn == NULL)
		{
			elog(ERROR,"We can't execute this SQL statement because we can't connect back to the masterDB");
		}
		
		/*
		 * We don't yet know what to do with a caller specified snapshot
		 */
		if (snapshot != InvalidSnapshot)
				elog(DEBUG1,"Snapshot valid... Help!");
		
		
		/*
		 * We need to make sure any work on the callback connection commits when
		 * we do, and rolls back when we rollback.  That means we need to make
		 * sure it's part of a transaction, so we need to send it a BEGIN.
		 */
		 
		/*
		 * txnOptions is used only if we send the query via PQsendMppQuery
		 * Otherwise, we need to do an explicit BEGIN.
		 */
		txnOptions = mppTxnOptions(true /* AutoBegin */);
		
		_SPI_current->processed = 0;
		_SPI_current->lastoid = InvalidOid;
		_SPI_current->tuptable = NULL;
		
		/* 
		 * If the statement has arguments (parameters), they are being passed to us
		 * in "internal" form, but libpq calls want them in "external" form.
		 * So, we need to convert each parameter using the correct function for it's 
		 * datatype (specified by oid).
		 * 
		 * We convert them to "text", rather than trying to deal with "binary".
		 */
		if (nargs > 0)
		{
			paramValues = palloc(nargs*sizeof(char*));
			paramLengths = palloc(nargs*sizeof(int));
			paramFormats = palloc(nargs*sizeof(int));

			Assert(plan->argtypes != NULL);

			for (i=0;i<nargs;i++)
			{
				char	   *outputbytes=0;
				Oid			typsend;
				bool		typisvarlena;
				FmgrInfo finfo;
				Datum val;
				
				if (Nulls && Nulls[i] == 'n')
				{
					outputbytes = "";
				}
				else
				{
		
					/* Get functionId of output function */
					getTypeOutputInfo(plan->argtypes[i], &typsend, &typisvarlena);
					fmgr_info(typsend, &finfo);
					 
					/* Call the attribute type's output converter. */
					if (finfo.fn_nargs != 1)
						elog(WARNING,"Function wants more than one arg");
						
					if (typisvarlena)
						val = PointerGetDatum(PG_DETOAST_DATUM(Values[i]));
					else
						val = Values[i];
						
					outputbytes = DatumGetCString(FunctionCall1(&finfo, val));
				}
																		 
				paramValues[i] = outputbytes;
				paramLengths[i] = strlen(outputbytes);
				paramFormats[i] = 0;   // 0 = text, 1 = binary
				
				
			}
		}
		
		
		/* 
		 * It seems that we should use the special PQsendMppQuery, so that we can include the
		 * snapshot info that we want to use with the request.
		 * 
		 * But, this has some issues:  Right now, PQsendMppQuery doesn't support us sending parameters
		 * (it assumes the statement is an MPPEXEC, and the parameters will be encoded special)
		 * So, the only way we can use it is if we replace the parameter markers with the actual
		 * string.
		 * 
		 * And, it doesn't handle prepared statements, it always does both a prepare and execute.
		 * That's not very efficient, but it gets the right answer.
		 * 
		 * 
		 * 
		 */
		if (gp_use_snapshop_during_callback) 
		{
			char *serializedSnapshot;
			int serializedSnapshotlen;
			const char * query = plan->query;
			StringInfoData newquery;
			char * query_text;
			int query_text_len;
			
			
			/* Loop backwards, so we handle $10 before $1, and don't get confused */
			for (i=nargs-1;i>=0;i--)
			{	
				char parmmarker[10];
				char * parmloc;
				
				initStringInfo(&newquery);
				
				sprintf(parmmarker,"$%d",i+1);
				
				parmloc = strstr(query,parmmarker);
				
				if (parmloc == NULL)
					elog(ERROR,"Could not find parameter marker in SQL query");
					
				appendBinaryStringInfo(&newquery, query, (parmloc-query));
				if (Nulls && Nulls[i] == 'n')
					appendStringInfo(&newquery, " NULL");
				else
					appendStringInfo(&newquery, "'%s'",paramValues[i]);
				appendStringInfo(&newquery, "::%s ",format_type_be(plan->argtypes[i]));
				appendStringInfo(&newquery, parmloc+strlen(parmmarker));
					
					
				elog(DEBUG1,"%s",newquery.data);
				 
				query = pstrdup(newquery.data);
				
			}

			if (snapshot == InvalidSnapshot)
			{
				/*
				 * Default read_only behavior is to use the entry-time
				 * ActiveSnapshot; if read-write, grab a full new snap.
				 */
				if (read_only)
					ActiveSnapshot = CopySnapshot(saveActiveSnapshot);
				else
					ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
			}
			else
			{
				elog(DEBUG1,"Snapshot is valid");
				/*
				 * We interpret read_only with a specified snapshot to be
				 * exactly that snapshot, but read-write means use the
				 * snap with advancing of command ID.
				 */
				ActiveSnapshot = CopySnapshot(snapshot);
				if (!read_only)
					ActiveSnapshot->curcid = GetCurrentCommandId();
			}
			
			if (Mpp_role == MPP_ROLE_EXECUTE)
			{
			
				/*
				 * Yuk... We are on the QE, which means our snapshot and transaction ID aren't 
				 * the same as the QD.
				 * 
				 * But, we can find the info as originally sent to us from the QD.
				 * 
				 * Is GetCurrentCommandId() right?  Or should this be based on the one from the QD,
				 * moved forward?
				 */

				char * p;
				TransactionId currentTransactionId = QDSentXID; // The global XID from the QD
				CommandId currentCommandId = GetCurrentCommandId();  // or QDSentSnapshot->curcid++;
		
				serializedSnapshotlen = sizeof(TransactionId)*3 + 
												sizeof(uint32) + /*xcnt*/
												snapshot->xcnt*sizeof(TransactionId) + sizeof(CommandId);
				serializedSnapshot = palloc(serializedSnapshotlen);
				p = serializedSnapshot;
				memcpy(p,&currentTransactionId,sizeof(TransactionId)); p += sizeof(TransactionId);
				memcpy(p,&snapshot->xmin,sizeof(TransactionId)); p += sizeof(TransactionId);
				memcpy(p,&snapshot->xmax,sizeof(TransactionId)); p += sizeof(TransactionId);
				memcpy(p,&snapshot->xcnt,sizeof(uint32)); p += sizeof(uint32);
				for (i = 0; i < snapshot->xcnt; i++)
				{
					memcpy(p,&snapshot->xip[i],sizeof(TransactionId)); p += sizeof(TransactionId);
				}
				memcpy(p,&currentCommandId,sizeof(CommandId)); p += sizeof(CommandId);
			}
			else
			{
				serializedSnapshot = serializeSnapshot(&serializedSnapshotlen);
				
			}
	

			query_text = PQbuildMppQueryString(
				query, strlen(query)+1, 
				NULL, 0, NULL, 0, NULL, 0, NULL, 0,
				serializedSnapshot, serializedSnapshotlen, 
				txnOptions, gp_command_count, -1, 
				GetSessionUserId(), superuser_arg(GetSessionUserId()), GetOuterUserId(), superuser_arg(GetOuterUserId()), GetUserId(),
				&query_text_len);
			 
			elog(DEBUG1,"Sending via special PQsendMppQuery");
			if (PQsendMppQuery_shared(_QD_conn, (char *)query_text, query_text_len) == 0)
			{
				/* Note the error. */
				elog(ERROR,"Command could not be sent to QD %s", PQerrorMessage(_QD_conn));
			}
	
			pfree(query_text);
			
		}
		/* End special code to use PQsendMppQuery */
		else
		{
			/*
			 * We need to make sure any work on the callback connection commits when
			 * we do, and rolls back when we rollback.  That means we need to make
			 * sure it's part of a transaction, so we need to send it a BEGIN.
			 * 
			 * Better would be to execute the command with the same snapshot info
			 * it would have had if it had been run on the QD from the original connection.
			 */
			elog(NOTICE,"gp_use_snapshop_during_callback is false, using standard calls");
			
			if (PQtransactionStatus(_QD_conn) != PQTRANS_INTRANS) /* Only BEGIN if not already in txn */
			{				
				char beginStmt[100];

				int ExplicitIsoLevel;
				bool ExplicitReadOnly;
				unpackMppTxnOptions(txnOptions, &ExplicitIsoLevel, &ExplicitReadOnly);
				
				sprintf(beginStmt, "BEGIN ISOLATION LEVEL %s, READ %s", 
					IsoLevelAsUpperString(ExplicitIsoLevel),
					(ExplicitReadOnly ? "ONLY" : "WRITE"));

				elog(DEBUG1, "Sending %s.",beginStmt);

				/* kick off an explict BEGIN */
				pgres = PQexec(_QD_conn, beginStmt);
				if (PQresultStatus(pgres) != PGRES_COMMAND_OK)
			    {
			        elog(ERROR, PQerrorMessage(_QD_conn)); 
			    }
			    PQclear(pgres);
			    /* 
			     * I think this wipes out any unnamed prepared statement, but I'm not sure
			     * let's play it safe.
			     */
			    _QD_currently_prepared_stmt = NULL;
			}
			
			/*
			 * We need to execute the statement that was previously prepared.  But,
			 * if we called back from QE to QD to prepare it, the QD might have freed the 
			 * plan if anything else was run between then and now.  We keep track of 
			 * this via "currently_prepared_stmt".  If that isn't the statement we 
			 * want to execute, we re-do the prepare.
			 * 
			 * Note that this code relys on the "Plan" passed to us having the 
			 * original source of the SQL statement in it.
			 */
			if (_QD_currently_prepared_stmt == NULL ||	
				strcmp(_QD_currently_prepared_stmt, plan->query) !=0)
			{
				elog(DEBUG1,"Re-preparing plan");
				pgres = PQprepare(_QD_conn,
	                "",
	                plan->query,
	                plan->nargs,
	                plan->argtypes);
	                    
		        if (PQresultStatus(pgres) != PGRES_COMMAND_OK)
			    {
			        elog(ERROR, PQerrorMessage(_QD_conn));
			        PQclear(pgres);
			        
			    }
			    _QD_currently_prepared_stmt = pstrdup(plan->query);
			}
                    
        	if (0 == PQsendQueryPrepared(_QD_conn,
                         "",
                         nargs,  
                         paramValues,
                         paramLengths,
                         paramFormats,
                         0 /*resultFormat*/) )
            {
            	/* Note the error. */
				elog(ERROR,"Command could not be sent to QD %s", PQerrorMessage(_QD_conn));
            }  
		}
           
        /* 
         * Ok, query sent.  Let's wait for the results.
         * We do this loop, because there is some chance the query was either multiple sql statements,
         * or another call to a stored proc, so there might be multiple result sets.
         * We only look at the last, but we want to make sure we keep all the errors if there
         * are multiple ones.
         * 
         * This is the same as PQexecFinish, but that's a static routine in fe-exec.c
         * 
         */
		{
			PGresult   *result;
			 
		
			pgres = NULL;
			while ((result = PQgetResult(_QD_conn)) != NULL)
			{
				if (pgres)
				{
					if (pgres->resultStatus == PGRES_FATAL_ERROR &&
						result->resultStatus == PGRES_FATAL_ERROR)
					{
						pqCatenateResultError(pgres, result->errMsg);
						PQclear(result);
						result = pgres;
		
						/*
						 * Make sure PQerrorMessage agrees with concatenated result
						 */
						resetPQExpBuffer(&_QD_conn->errorMessage);
						appendPQExpBufferStr(&_QD_conn->errorMessage, result->errMsg);
					}
					else
						PQclear(pgres);
				}
				pgres = result;
				if (result->resultStatus == PGRES_COPY_IN ||
					result->resultStatus == PGRES_COPY_OUT ||
					_QD_conn->status == CONNECTION_BAD)
					break;
			}
		
			 
		}       
		
		/*
		 * Ok, got our results back.  Let's first make sure the query executed ok, otherwise
		 * let's just bail out.
		 */ 
        if (PQresultStatus(pgres) != PGRES_COMMAND_OK && PQresultStatus(pgres) != PGRES_TUPLES_OK)
	    {
	    	PQclear(pgres);
	        elog(ERROR, PQerrorMessage(_QD_conn));
	        
	        return SPI_ERROR_ARGUMENT;
	    }

		/*
		 * If the command was a SELECT that returned rows, we will get PGRES_TUPLES_OK
		 * 
		 * In that case, we need to convert the results into the form that SPI routine callers expect.
		 */
        if (PQresultStatus(pgres) == PGRES_TUPLES_OK)
        {
        	int i;
        	int j;
        	HeapTuple tuple;
        	Datum *values;
			char *nulls;
			TupleDesc tempTupleDesc;
			TupleTableSlot slot;
			Oid			typrecv;
			Oid			typioparam;
			FmgrInfo finfo;
			
        	
        	_SPI_current->processed = PQntuples(pgres);
        	
        	/*
        	 * Ok, libpq gives us back fields in "external" format (our choice of binary or text), rather than
        	 * "internal" format.  But SPI routines return data in internal format, as well as returning 
        	 * a Tuple Descriptor to match.   SPI_tuptable is was gets returned to the caller, and while we
        	 * are working in this routine, its at _SPI_current->tuptable. 
        	 * 
        	 * First, we need to make a compatible Tuple Descriptor to match the results
        	 */
        	tempTupleDesc = CreateTemplateTupleDesc(PQnfields(pgres), false);
        	
        	for(i=0;i<PQnfields(pgres);i++)
        		TupleDescInitEntry(tempTupleDesc, (AttrNumber) i+1, PQfname(pgres,i), PQftype(pgres,i), PQfmod(pgres,i), 0 /*attdim*/);
        
        	if (tempTupleDesc->tdtypeid == RECORDOID && tempTupleDesc->tdtypmod < 0)
				assign_record_type_typmod(tempTupleDesc);
				
        	/* This sets up and initializes tuptable */
        	spi_dest_startup(NULL, 0, tempTupleDesc);
        	
        	
			nulls = palloc(PQnfields(pgres)+1);
			values = palloc(PQnfields(pgres)*sizeof(Datum));
			
			/*
			 * Ok, now get each field that's being returned.  We need to use it's "input" function (based on
			 * it's datatype) to convert from the "external" representation libpq gives us, back to the
			 * "internal" representation needed in the tuptable.
			 */
        	
        	for(i=0;i<PQntuples(pgres);i++)
        	{
        		for(j=0;j<PQnfields(pgres);j++)
        		{
        			char * v;
        			nulls[j] = ' ';
        			if (PQgetisnull(pgres,i,j))
        			{
        				nulls[j] = 'n';
        				values[j] = 0;
        			}
        			else
        			{
	        			v = PQgetvalue(pgres,i,j);
	        			
						/* Get functionId of input function */
						getTypeInputInfo(tempTupleDesc->attrs[j]->atttypid, &typrecv, &typioparam);
						fmgr_info(typrecv, &finfo);
	        			
						if (finfo.fn_nargs == 1)
							values[j] = FunctionCall1(&finfo,
										  CStringGetDatum(v));
					    else if (finfo.fn_nargs == 2)
					    	values[j] = FunctionCall2(&finfo,
										  CStringGetDatum(v),
									   ObjectIdGetDatum(typioparam));
						else
							values[j] = FunctionCall3(&finfo,
										  CStringGetDatum(v),
									   ObjectIdGetDatum(typioparam),
									Int32GetDatum(-1));
        			}
        			 
        		}
        		/*
        		 * We've converted one row into the values[] and nulls[] arrays.
        		 * 
        		 * Now we can use heap_formtuple to make it into a tuple.
        		 */
        		tuple = heap_formtuple(tempTupleDesc, values, nulls);
        		/* Do we need to set a tuple oid? I don't think so */
				/*if (objectid != (Oid) 0)
					 HeapTupleSetOid(tuple, objectid);*/
					
				/*
				 * To get it into the tuptable we are building, we need to 
				 * call spi_printtup.  But that wants a Tuple Table Slot, not
				 * a heap tuple.  Lucky for us, one of the possible ways a Slot
				 * can be is just a pointer to a heap tuple.
				 * 
				 * So, let's make a dummy slot, point it at the heap tuple,
				 * and call spi_printtup to save it away
				 */
				memset(&slot,0,sizeof(TupleTableSlot));
				slot.tts_tuple = tuple;
				slot.tts_tupleDescriptor = tempTupleDesc;
				slot.tts_isempty = false;
				spi_printtup(&slot, NULL);
				
        	}
        	/*
        	 * Ok, _SPI_current->tuptable now has the results in it.
        	 */
        	_SPI_current->lastoid = PQoidValue(pgres);
        	
        	my_res = SPI_OK_SELECT;
	 
        } 
        else
        {
        	/*
        	 * We got back PGRES_COMMAND_OK, so this was not a SELECT statement.
        	 * 
        	 * It could be a utility statement, or an INSERT/UPDATE/DELETE.
        	 * 
        	 * We need to differentiate for the return info.
        	 * 
        	 */
        	char * cmdstatus;
        	 
        	my_res = SPI_OK_UTILITY;
        	
        	cmdstatus = PQcmdStatus(pgres);
        	if (strncmp(cmdstatus, "INSERT", 6) == 0)
        		my_res = SPI_OK_INSERT;
			else if (strncmp(cmdstatus, "DELETE", 6) == 0)
				my_res = SPI_OK_DELETE;
			else if (strncmp(cmdstatus, "UPDATE", 6) == 0)
				my_res = SPI_OK_UPDATE;
			else if (strncmp(cmdstatus, "FETCH", 5) == 0)
				my_res = SPI_OK_FETCH;
 
        	_SPI_current->processed = atol(PQcmdTuples(pgres));
        	_SPI_current->lastoid = PQoidValue(pgres);
        	
        	
  
        }
       
        /*
         * Any results have now been converted and stored away, so
         * we can free the libpq results.
         */
        
    	 
    	PQclear(pgres);
    	
    	/*
    	 * We are done. 
    	 */ 
    	
        ActiveSnapshot = saveActiveSnapshot;
        SPI_processed = _SPI_current->processed;
		SPI_lastoid = _SPI_current->lastoid;
		SPI_tuptable = _SPI_current->tuptable;
        elog(DEBUG1,"_SPI_execute_plan done %d", my_res);
        return my_res;
        
	}
	
	/* 
	 * In the normal case, where we are on the QD, we can do the normal
	 * PostgreSQL thing and run the command ourselves 
	 */
	
	elog(DEBUG1,"_SPI_execute_plan local: %s",plan->query);
	/*elog_node_display(DEBUG5,"_SPI_execute_plan queryTree",plan->qtlist,true);*/
	
	debug_query_string = plan->query;
	
	PG_TRY();
	{
		List	   *query_list_list = plan->qtlist;
		ListCell   *plan_list_item = list_head(plan->ptlist);
		ListCell   *query_list_list_item;
		ErrorContextCallback spierrcontext;
		int			nargs = plan->nargs;
		ParamListInfo paramLI;

		/* Convert parameters to form wanted by executor */
		if (nargs > 0)
		{
			int			k;

			/* sizeof(ParamListInfoData) includes the first array element */
			paramLI = (ParamListInfo) palloc(sizeof(ParamListInfoData) +
									   (nargs - 1) *sizeof(ParamExternData));
			paramLI->numParams = nargs;

			for (k = 0; k < nargs; k++)
			{
				ParamExternData *prm = &paramLI->params[k];

				prm->value = Values[k];
				prm->isnull = (Nulls && Nulls[k] == 'n');
				prm->pflags = 0;
				prm->ptype = plan->argtypes[k];
			}
		}
		else
			paramLI = NULL;

		/*
		 * Setup error traceback support for ereport()
		 */
		spierrcontext.callback = _SPI_error_callback;
		spierrcontext.arg = (void *) plan->query;
		spierrcontext.previous = error_context_stack;
		error_context_stack = &spierrcontext;

		foreach(query_list_list_item, query_list_list)
		{
			List	   *query_list = lfirst(query_list_list_item);
			ListCell   *query_list_item;

			foreach(query_list_item, query_list)
			{
				Query	   *queryTree = (Query *) lfirst(query_list_item);
				Plan	   *planTree;
				QueryDesc  *qdesc;
				DestReceiver *dest;

				planTree = lfirst(plan_list_item);
				plan_list_item = lnext(plan_list_item);

				_SPI_current->processed = 0;
				_SPI_current->lastoid = InvalidOid;
				_SPI_current->tuptable = NULL;

				if (queryTree->commandType == CMD_UTILITY)
				{
					if (IsA(queryTree->utilityStmt, CopyStmt))
					{
						CopyStmt   *stmt = (CopyStmt *) queryTree->utilityStmt;

						if (stmt->filename == NULL)
						{
							my_res = SPI_ERROR_COPY;
							goto fail;
						}
					}
					else if (IsA(queryTree->utilityStmt, DeclareCursorStmt) ||
							 IsA(queryTree->utilityStmt, ClosePortalStmt) ||
							 IsA(queryTree->utilityStmt, FetchStmt))
					{
						my_res = SPI_ERROR_CURSOR;
						goto fail;
					}
					else if (IsA(queryTree->utilityStmt, TransactionStmt))
					{
						my_res = SPI_ERROR_TRANSACTION;
						goto fail;
					}
				}

				if (read_only && !QueryIsReadOnly(queryTree))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					/* translator: %s is a SQL statement name */
					   errmsg("%s is not allowed in a non-volatile function",
							  CreateQueryTag(queryTree))));

				/*
				 * If not read-only mode, advance the command counter before
				 * each command.
				 */
				if (!read_only)
					CommandCounterIncrement();

				dest = CreateDestReceiver(queryTree->canSetTag ? DestSPI : DestNone,
										  NULL);

				if (snapshot == InvalidSnapshot)
				{
					/*
					 * Default read_only behavior is to use the entry-time
					 * ActiveSnapshot; if read-write, grab a full new snap.
					 */
					if (read_only)
						ActiveSnapshot = CopySnapshot(saveActiveSnapshot);
					else
						ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
				}
				else
				{
					/*
					 * We interpret read_only with a specified snapshot to be
					 * exactly that snapshot, but read-write means use the
					 * snap with advancing of command ID.
					 */
					ActiveSnapshot = CopySnapshot(snapshot);
					if (!read_only)
						ActiveSnapshot->curcid = GetCurrentCommandId();
				}

				if (queryTree->commandType == CMD_UTILITY)
				{
					ProcessUtility(queryTree->utilityStmt, paramLI,
								   dest, NULL);
					/* Update "processed" if stmt returned tuples */
					if (_SPI_current->tuptable)
						_SPI_current->processed = _SPI_current->tuptable->alloced - _SPI_current->tuptable->free;
					res = SPI_OK_UTILITY;
				}
				else
				{
					qdesc = CreateQueryDesc(queryTree, planTree,
											ActiveSnapshot,
											crosscheck_snapshot,
											dest,
											paramLI, false);
					res = _SPI_pquery(qdesc,
									  queryTree->canSetTag ? tcount : 0);
					FreeQueryDesc(qdesc);
				}
				FreeSnapshot(ActiveSnapshot);
				ActiveSnapshot = NULL;

				/*
				 * The last canSetTag query sets the status values returned to
				 * the caller.	Be careful to free any tuptables not returned,
				 * to avoid intratransaction memory leak.
				 */
				if (queryTree->canSetTag)
				{
					my_processed = _SPI_current->processed;
					my_lastoid = _SPI_current->lastoid;
					SPI_freetuptable(my_tuptable);
					my_tuptable = _SPI_current->tuptable;
					my_res = res;
				}
				else
				{
					SPI_freetuptable(_SPI_current->tuptable);
					_SPI_current->tuptable = NULL;
				}
				/* we know that the receiver doesn't need a destroy call */
				if (res < 0)
				{
					my_res = res;
					goto fail;
				}
			}
		}

fail:

		/*
		 * Pop the error context stack
		 */
		error_context_stack = spierrcontext.previous;
	}
	PG_CATCH();
	{
		/* Restore global vars and propagate error */
		ActiveSnapshot = saveActiveSnapshot;
		PG_RE_THROW();
	}
	PG_END_TRY();

	ActiveSnapshot = saveActiveSnapshot;

	/* Save results for caller */
	SPI_processed = my_processed;
	SPI_lastoid = my_lastoid;
	SPI_tuptable = my_tuptable;

	/* tuptable now is caller's responsibility, not SPI's */
	_SPI_current->tuptable = NULL;

	/*
	 * If none of the queries had canSetTag, we return the last query's result
	 * code, but not its auxiliary results (for backwards compatibility).
	 */
	if (my_res == 0)
		my_res = res;

	return my_res;
}

static int
_SPI_pquery(QueryDesc *queryDesc, long tcount)
{
	int			operation = queryDesc->operation;
	int			res;

	switch (operation)
	{
		case CMD_SELECT:
			if (queryDesc->parsetree->into)		/* select into table? */
				res = SPI_OK_SELINTO;
			else if (queryDesc->dest->mydest != DestSPI)
			{
				/* Don't return SPI_OK_SELECT if we're discarding result */
				res = SPI_OK_UTILITY;
			}
			else
				res = SPI_OK_SELECT;
			break;
		case CMD_INSERT:
			if (queryDesc->parsetree->returningList)
				res = SPI_OK_INSERT_RETURNING;
			else
				res = SPI_OK_INSERT;
			break;
		case CMD_DELETE:
			if (queryDesc->parsetree->returningList)
				res = SPI_OK_DELETE_RETURNING;
			else
				res = SPI_OK_DELETE;
			break;
		case CMD_UPDATE:
			if (queryDesc->parsetree->returningList)
				res = SPI_OK_UPDATE_RETURNING;
			else
				res = SPI_OK_UPDATE;
			break;
		default:
			return SPI_ERROR_OPUNKNOWN;
	}

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ResetUsage();
#endif

	if (!cdbpathlocus_querysegmentcatalogs)
	AfterTriggerBeginQuery();

	ExecutorStart(queryDesc, 0);

	ExecutorRun(queryDesc, ForwardScanDirection, tcount);

	_SPI_current->processed = queryDesc->estate->es_processed;
	_SPI_current->lastoid = queryDesc->estate->es_lastoid;

	if ((res == SPI_OK_SELECT || queryDesc->parsetree->returningList) &&
		queryDesc->dest->mydest == DestSPI)
	{
		if (_SPI_checktuples())
			elog(ERROR, "consistency check on SPI tuple count failed");
	}

	if (!cdbpathlocus_querysegmentcatalogs)
	/* Take care of any queued AFTER triggers */
	AfterTriggerEndQuery(queryDesc->estate);

	ExecutorEnd(queryDesc);
	
	_SPI_current->processed = queryDesc->es_processed; /* Mpp: Dispatched queries fill in this at Executor End */
	_SPI_current->lastoid = queryDesc->es_lastoid;
	
#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ShowUsage("SPI EXECUTOR STATS");
#endif

	return res;
}

/*
 * _SPI_error_callback
 *
 * Add context information when a query invoked via SPI fails
 */
static void
_SPI_error_callback(void *arg)
{
	const char *query = (const char *) arg;
	int			syntaxerrposition;

	/*
	 * If there is a syntax error position, convert to internal syntax error;
	 * otherwise treat the query as an item of context stack
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(query);
	}
	else
		errcontext("SQL statement \"%s\"", query);
}

/*
 * _SPI_cursor_operation()
 *
 *	Do a FETCH or MOVE in a cursor
 */
static void
_SPI_cursor_operation(Portal portal, bool forward, long count,
					  DestReceiver *dest)
{
	long		nfetched;
	elog(DEBUG1,"SPI_cursor_operation");

	/* Check that the portal is valid */
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	/* Push the SPI stack */
	if (_SPI_begin_call(true) < 0)
		elog(ERROR, "SPI cursor operation called while not connected");

	/* Reset the SPI result (note we deliberately don't touch lastoid) */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* Run the cursor */
	nfetched = PortalRunFetch(portal,
							  forward ? FETCH_FORWARD : FETCH_BACKWARD,
							  count,
							  dest);

	/*
	 * Think not to combine this store with the preceding function call. If
	 * the portal contains calls to functions that use SPI, then SPI_stack is
	 * likely to move around while the portal runs.  When control returns,
	 * _SPI_current will point to the correct stack entry... but the pointer
	 * may be different than it was beforehand. So we must be sure to re-fetch
	 * the pointer after the function call completes.
	 */
	_SPI_current->processed = nfetched;

	if (dest->mydest == DestSPI && _SPI_checktuples())
		elog(ERROR, "consistency check on SPI tuple count failed");

	/* Put the result into place for access by caller */
	SPI_processed = _SPI_current->processed;
	SPI_tuptable = _SPI_current->tuptable;

	/* tuptable now is caller's responsibility, not SPI's */
	_SPI_current->tuptable = NULL;

	/* Pop the SPI stack */
	_SPI_end_call(true);
}


static MemoryContext
_SPI_execmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->execCxt);
}

static MemoryContext
_SPI_procmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->procCxt);
}

/*
 * _SPI_begin_call: begin a SPI operation within a connected procedure
 */
static int
_SPI_begin_call(bool execmem)
{
	if (_SPI_curid + 1 != _SPI_connected)
		return SPI_ERROR_UNCONNECTED;
	_SPI_curid++;
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(ERROR, "SPI stack corrupted");

	if (execmem)				/* switch to the Executor memory context */
		_SPI_execmem();

	return 0;
}

/*
 * _SPI_end_call: end a SPI operation within a connected procedure
 *
 * Note: this currently has no failure return cases, so callers don't check
 */
static int
_SPI_end_call(bool procmem)
{
	/*
	 * We're returning to procedure where _SPI_curid == _SPI_connected - 1
	 */
	_SPI_curid--;

	if (procmem)				/* switch to the procedure memory context */
	{
		_SPI_procmem();
		/* and free Executor memory */
		MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
	}

	return 0;
}

static bool
_SPI_checktuples(void)
{
	uint32		processed = _SPI_current->processed;
	SPITupleTable *tuptable = _SPI_current->tuptable;
	bool		failed = false;

	if (tuptable == NULL)		/* spi_dest_startup was not called */
		failed = true;
	else if (processed != (tuptable->alloced - tuptable->free))
		failed = true;

	return failed;
}

static _SPI_plan *
_SPI_copy_plan(_SPI_plan *plan, int location)
{
	_SPI_plan  *newplan;
	MemoryContext oldcxt;
	MemoryContext plancxt;
	MemoryContext parentcxt;
	elog(DEBUG1,"_SPI_copy_plan");

	/* Determine correct parent for the plan's memory context */
	if (location == _SPI_CPLAN_PROCXT)
		parentcxt = _SPI_current->procCxt;
	else if (location == _SPI_CPLAN_TOPCXT)
		parentcxt = TopMemoryContext;
	else
		/* (this case not currently used) */
		parentcxt = CurrentMemoryContext;

	/*
	 * Create a memory context for the plan.  We don't expect the plan to be
	 * very large, so use smaller-than-default alloc parameters.
	 */
	plancxt = AllocSetContextCreate(parentcxt,
									"SPI Plan",
									ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE,
									ALLOCSET_SMALL_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(plancxt);

	/* Copy the SPI plan into its own context */
	newplan = (_SPI_plan *) palloc(sizeof(_SPI_plan));
	newplan->plancxt = plancxt;
	newplan->query = pstrdup(plan->query);
	newplan->qtlist = (List *) copyObject(plan->qtlist);
	newplan->ptlist = (List *) copyObject(plan->ptlist);
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;
		
	newplan->run_via_callback_to_qd = plan->run_via_callback_to_qd;

	MemoryContextSwitchTo(oldcxt);

	return newplan;
}

/*
 * This is apparently only used by libcdbtest, in cdbexecplan.c
 * 
 * Which in turn, is only used for testing.  At the moment, libcdbtest doesn't appear to work,
 * so I don't think this is ever called.
 */
int
SPI_execute_cdb(const char *src, Plan *pPlan, Query *pQuery, bool read_only, int tcount)
{
	_SPI_plan	plan;
	int			res;
	List	   *query_list = NIL;

	if (src == NULL || pPlan == NULL || pQuery == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	plan.plancxt = NULL;		/* doesn't have own context */
	plan.query = src;
	plan.nargs = 0;
	plan.argtypes = NULL;
	plan.ptlist = NIL;
	plan.qtlist = NIL;

	plan.ptlist = lappend(plan.ptlist, pPlan);

	query_list = lappend(query_list, pQuery);
	plan.qtlist = lappend(plan.qtlist, query_list);

	res = _SPI_execute_plan(&plan, NULL, NULL,
							InvalidSnapshot, InvalidSnapshot,
							read_only, tcount);

	_SPI_end_call(true);
	return res;
}
