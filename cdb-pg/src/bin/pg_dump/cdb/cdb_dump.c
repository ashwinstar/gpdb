/*-------------------------------------------------------------------------
 *
 * cdb_dump.c
 *	  cdb_dump is a utility for dumping out a cdb cluster of postgres databases
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include <unistd.h>
#include <signal.h>
#include <regex.h>
#include <ctype.h>
#include <pthread.h>
#include "getopt_long.h"
#include "pqexpbuffer.h"
#include "libpq-fe.h"
#include "fe-auth.h"
#include "cdb_table.h"
#include "cdb_dump_util.h"
#include "cdb_backup_state.h"
#include "cdb_dump.h"


/* This is necessary on platforms where optreset variable is not available.
 * Look at "man getopt" documentation for details.
 */
#ifndef HAVE_OPTRESET
int                     optreset;
#endif


/*
 * static helper functions.  See each function body for a description.
 */
static char* addPassThroughParm( char Parm, const char* pszValue, char* pszPassThroughParmString );
static bool createThreadParmArray( int nCount, ThreadParmArray* pParmAr);
static void decrementFinishedLaunchCount(void);
static bool doInitialActions(PGconn* pConn, const char* pszDBName);
static bool execCommit( PGconn* pConn );
static bool	fillInputOptions(int argc, char** argv, InputOptions* pInputOpts);
static void freeThreadParmArray(ThreadParmArray* pParmAr);
static void help(const char* pszProgName);
static void installSignalHandlers(void);
static void myHandler(SIGNAL_ARGS);
static bool parmValNeedsQuotes( const char* Value );
static int reportBackupResults( const char* pszReportDirectory, const ThreadParmArray* pParmAr );
static void spinOffThreads( PGconn* pConn, InputOptions* pInputOpts, const SegmentInstanceArray* pSegInstAr, 
							ThreadParmArray* pParmAr );
static void* threadProc(void *arg);
static void getDatabaseConnections(PGconn *conn, PGconn ***conn_dbs, int *numberOfDbs, InputOptions *inputOpts);
static PGresult *executeQuery(PGconn *conn, const char *query);

/*
 * static and extern global variables left over from pg_dump
 */
static char* progname;
extern char* optarg;
extern int	optind, opterr;
static int	disable_triggers = 0;
static bool	g_verbose;
static bool	dumpData;			/* dump data using proper insert strings */
static bool	attrNames;			/* put attr names into insert strings */
static bool	schemaOnly;
static bool	dataOnly;
static bool	aclsSkip;
static char* selectTableName = NULL;	/* name of a single table to dump */
static char* selectSchemaName = NULL;	/* name of a single schema to dump */
static char	g_opaque_type[10];	/* name for the opaque type */
static char	g_comment_start[10];
static char	g_comment_end[10];
static bool bForcePassword = false;
static bool bIgnoreVersion = false;

/*
 * Thread synchronization variables
 */
static int nThreadsFinishedLaunch;
static pthread_cond_t MyCondVar = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t MyMutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_b_SendCancelMessage = false;
static const char* pszAgent = "mpp_dump_agent";

int main( int argc, char** argv)
{
	PGconn* pConn	= NULL;
    int failCount    = -1;
	char szErrortmp[PQERRORMSG_LENGTH];
	
	/* This struct holds the values of the command line parameters */
	InputOptions			inputOpts;
	/* This struct holds an array of the segment instance pairs from the head mpp tables */
	SegmentInstanceArray	segInstAr;
	/* This struct holds an array of the thread parameters */
	ThreadParmArray			parmAr;
	PGconn **conn_dbs;
    int    numOfConns, i;
    
	memset( &inputOpts, 0, sizeof(inputOpts) );
	memset( &segInstAr, 0, sizeof(segInstAr) );
	memset( &parmAr, 0, sizeof(parmAr) );
	
	/* Parse command line for options */
	write_err_msg("Analyzing command line options.\n" );
	if ( !fillInputOptions( argc, argv, &inputOpts ) )
		goto cleanup;
	
	/* Connect to database on the head*/
	write_err_msg( "Connecting to head instance on host %s port %s database %s.\n",
				StringNotNull( inputOpts.pszPGHost, "localhost"), 
				StringNotNull( inputOpts.pszPGPort, "5432"),
				StringNotNull( inputOpts.pszDBName, fe_getauthname(szErrortmp) ) );
						
	pConn = MakeConnectionToDatabase(inputOpts.pszDBName, inputOpts.pszPGHost, 
									inputOpts.pszPGPort, inputOpts.pszUserName, 
									bForcePassword, bIgnoreVersion, false);
	if ( pConn == NULL )
		goto cleanup;

	/* Read mpp segment/instance configuration from the head */
	write_err_msg( "Reading mpp configuration info from head instance database.\n");
	if ( !GetSegmentInstanceArray(pConn, inputOpts.pszDBName, &inputOpts.set, &segInstAr, false) )
		goto cleanup;

    /* get connection to all databases of QD */
    getDatabaseConnections(pConn, &conn_dbs, &numOfConns, &inputOpts);

    for (i=0; i<numOfConns; i++)
    {        
	    /* Begin a transaction on the head and lock all mpp managed tables */
	    if ( !doInitialActions(conn_dbs[i], conn_dbs[i]->dbName) )
		   goto cleanup;
    }
	/* Install the SIGINT and SIGTERM handlers */  
	installSignalHandlers();
	
	/* Create the threads to talk to each of the databases being backed up.
	 * Wait for them to finish.  Inside this function, when all threads have started
	 * the transaction that we began in doInitialActions is committed. 
	 */ 
	spinOffThreads(pConn, &inputOpts, &segInstAr, &parmAr);
	
	write_err_msg( "%s: All remote %s programs are finished.\n", GetTimeNow(), pszAgent );
			
	/* Produce results report */
	failCount = reportBackupResults( inputOpts.pszReportDirectory, &parmAr );
	
	/* Clean-up memory allocated */
cleanup:	
	freeThreadParmArray( &parmAr );
	FreeSegmentInstanceArray( &segInstAr );
	FreeInputOptions( &inputOpts );
	
	/* Clean-up synchronization variables */
	pthread_mutex_destroy( &MyMutex );
	pthread_cond_destroy( &MyCondVar );
	
	if ( pConn != NULL )
		PQfinish(pConn);
    for (i=0; i<numOfConns; i++)
        PQfinish(conn_dbs[i]);
        
	exit(failCount);
}

/*
 * addPassThroughParm: this function adds to the string of pass-through parameters.  These get sent to each
 * backend and get passed to the mpp_dump_agent program.
 */
char* addPassThroughParm( char Parm, const char* pszValue, char* pszPassThroughParmString )
{
	char*	pszRtn;
	bool	bFirstTime = (pszPassThroughParmString == NULL);
	
	if ( pszValue != NULL )
	{
		if ( parmValNeedsQuotes( pszValue ) )
		{
			if ( bFirstTime )
				pszRtn = MakeString( "-%c \"%s\"", Parm, pszValue );
			else
				pszRtn = MakeString( "%s -%c \"%s\"", pszPassThroughParmString, Parm, pszValue );
		}
		else
		{
			if ( bFirstTime )
				pszRtn = MakeString( "-%c %s", Parm, pszValue );
			else
				pszRtn = MakeString( "%s -%c %s", pszPassThroughParmString, Parm, pszValue );
		}
	}
	else
	{
		if ( bFirstTime )
			pszRtn = MakeString( "-%c", Parm );
		else
			pszRtn = MakeString( "%s -%c", pszPassThroughParmString, Parm );
	}
		
	return pszRtn; 
}

/*
 * createThreadParmArray: This function initializes the count and pData elements of the ThreadParmArray. 
 */
bool createThreadParmArray( int nCount, ThreadParmArray* pParmAr)
{
	int i;
	pParmAr->count = nCount;
	pParmAr->pData = (ThreadParm *)calloc( nCount, sizeof(ThreadParm) );
	if ( pParmAr->pData == NULL )
		return false;
	
	for ( i=0; i<nCount; i++ )
	{
		ThreadParm* p = &pParmAr->pData[i];
		
		p->thread				= 0;
		p->pSegInstData			= NULL;
		p->pOptionsData 		= NULL;
		p->bSuccess				= true;
		p->pszErrorMsg			= NULL;
		p->pszRemoteBackupPath	= NULL;
	}
	
	return true;
}

/*
 * decrementFinishedLaunchCount: This function handles the decrementing of the finished launch count.
 * It uses a mutex to protect the int variable.  If this is the last thread to get here, the variable
 * will be 0, and the condition variable is signalled, so the main threrad wakes up. 
 */
void decrementFinishedLaunchCount(void)
{
	pthread_mutex_lock( &MyMutex );
	
	nThreadsFinishedLaunch--;
	
	if ( nThreadsFinishedLaunch == 0 )
		pthread_cond_signal( &MyCondVar );
	
	pthread_mutex_unlock( &MyMutex );
}

/*
 * doInitialActions: This function uses the PGconn connection to begin a transaction. Then it reads an array 
 * of the mpp managed tables, along with their namespaces.  It then forms one LOCK TABLE 
 * statement using each of the tables in the array, and executes it.
 */
bool doInitialActions(PGconn* pConn, const char* pszDBName)
{
	bool		bRtn = false;
	PQExpBuffer	pQry = NULL;
	PGresult*	pRes = NULL;
	CDBBaseTableArray TableAr;
	int i;
	
	memset( &TableAr, 0, sizeof(TableAr) );
	
	write_err_msg( "Starting a transaction on head instance database %s.\n", pConn->dbName);
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry, "Begin;");
	pRes = PQexec(pConn, pQry->data);
	if ( pRes == NULL )
	{
		write_err_msg( "no result from server: %s", PQerrorMessage(pConn));
		goto cleanup;
	}
	
	if (PQresultStatus(pRes) != PGRES_COMMAND_OK)
	{
		write_err_msg( "bad result from server: %s", PQerrorMessage(pConn));
		goto cleanup;
	}

	PQclear(pRes);
    pRes = NULL;
	resetPQExpBuffer(pQry);
	
	write_err_msg( "Getting a list of mpp managed tables from mpp_basetable.\n");
	
	if ( !GetCDBBaseTableArray(pConn, pszDBName, &TableAr, NULL ) )
	{
		goto cleanup;	
	}
	
	if ( TableAr.count > 0 )
	{
		write_err_msg( "Issuing \"LOCK IN EXCLUSIVE MODE\" command for mpp managed tables on database %s.\n", pConn->dbName);
		
		// Now issue lock table statements for each table in BaseTablesAr
		appendPQExpBuffer(pQry, "LOCK TABLE");
		
		for ( i=0; i<TableAr.count; i++ )
		{
			if ( i > 0 )
				appendPQExpBuffer(pQry, ",");
			
			appendPQExpBuffer(pQry, " ");
			appendPQExpBuffer(pQry, TableAr.pData[i].pszNspName);
			appendPQExpBuffer(pQry, ".");
			appendPQExpBuffer(pQry, TableAr.pData[i].pszTableName);
		}
		
		appendPQExpBuffer(pQry, " IN EXCLUSIVE MODE");
		
		pRes = PQexec(pConn, pQry->data);
		if ( pRes == NULL )
		{
			write_err_msg("no result from server: %s", PQerrorMessage(pConn));
			goto cleanup;
		}
		
		if (PQresultStatus(pRes) != PGRES_COMMAND_OK)
		{
			write_err_msg("bad result from server: %s", PQerrorMessage(pConn));
			goto cleanup;
		}
        if (g_verbose)
            write_err_msg("Executing %s\n", pQry->data);
	}
	
	bRtn = true;

cleanup:

	if ( pRes != NULL )
		PQclear(pRes);
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);
	
	FreeCDBBaseTableArray( &TableAr );
	
	return bRtn;
}

/*
 * execCommit: This function simply issues a commit command using the pConn PGconn* parameter.
 */
bool execCommit( PGconn* pConn )
{
	bool		bRtn = false;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry,	" COMMIT" ); 

	pRes = PQexec(pConn, pQry->data);
	if ( pRes == NULL )
	{
		write_err_msg( "no result from server: %s\n", PQerrorMessage(pConn));
		goto cleanup;
	}
	
	if (PQresultStatus(pRes) != PGRES_COMMAND_OK)
	{
		write_err_msg( "bad result from server: %s\n", PQerrorMessage(pConn));
		goto cleanup;
	}
	
	bRtn = true;

cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);
	
	return bRtn;
}
		
/*
 * fillInputOptions: This function gets the command line options, and stores the values in the InputOptions structure.
 * Much of this code was copied from pg_dump.
 */
bool fillInputOptions( int argc, char** argv, InputOptions* pInputOpts )
{
	bool			bRtn = false;
	int				c;
	static int		auth = 0;
	char*			pszFormat = "p";
	bool			bOutputBlobs = false;
	bool			bOids = false;
	int				compressLevel	= -1;
	int				outputClean = 0;
	int				outputCreate = 0;
	int				outputNoOwner = 0;
	bool bSawCDB_S_Option;
	int lenCmdLineParms;
	int i;

	
	static struct option long_options[] = 
	{
		{"data-only", no_argument, NULL, 'a'},
		{"blobs", no_argument, NULL, 'b'},
		{"clean", no_argument, NULL, 'c'},
		{"create", no_argument, NULL, 'C'},
		{"file", required_argument, NULL, 'f'},
		{"format", required_argument, NULL, 'F'},
		{"inserts", no_argument, NULL, 'd'},
		{"attribute-inserts", no_argument, NULL, 'D'},
		{"column-inserts", no_argument, NULL, 'D'},
		{"host", required_argument, NULL, 'h'},
		{"ignore-version", no_argument, NULL, 'i'},
		{"no-reconnect", no_argument, NULL, 'R'},
		{"oids", no_argument, NULL, 'o'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"schema", required_argument, NULL, 'n'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"password", no_argument, NULL, 'W'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},
		{"compress", required_argument, NULL, 'Z'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},

		/*
		 * the following options don't have an equivalent short option
		 * letter, but are available as '-X long-name'
		 */
		{"use-set-session-authorization", no_argument, &auth, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},
		
		/*
		 * the following are mpp specific, and don't have an equivalent short option
		 */
		{"mpp-c", required_argument, NULL, 1},
		//{"mpp-cf", required_argument, NULL, 2},
		{"mpp-d", required_argument, NULL, 3},
		{"mpp-r", required_argument, NULL, 4},
		//{"mpp-s", required_argument, NULL, 5},
		{NULL, 0, NULL, 0}
	};
	
	int			optindex;

	/* Initialize option fields  */
	memset( pInputOpts, 0, sizeof(InputOptions) );
	pInputOpts->set.type		= SET_ALL;
	pInputOpts->set.count		= 0;
	pInputOpts->set.pData		= NULL;

	g_verbose = false;

	strcpy(g_comment_start, "-- ");
	g_comment_end[0] = '\0';
	strcpy(g_opaque_type, "opaque");

	dataOnly = schemaOnly = dumpData = attrNames = false;

	progname = (char *)get_progname(argv[0]);
	
	bSawCDB_S_Option = false;

	if (argc == 1)
	{
		help(progname);
		goto cleanup;
	}
	
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			goto cleanup;
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("mpp_dump (Bizgres MPP) " PG_VERSION);
			goto cleanup;
		}
	}
	
	// Record the command line parms as a string for documentation in the ouput report
	lenCmdLineParms = 0;
	for ( i=1; i<argc; i++ )
	{
		if ( i > 1 )
			lenCmdLineParms += 1;
		
		lenCmdLineParms += strlen( argv[i] );
		if ( parmValNeedsQuotes( argv[i] ) )
			lenCmdLineParms += 2;
	}
	
	pInputOpts->pszCmdLineParms = (char *)malloc( lenCmdLineParms + 1 );
	if ( pInputOpts->pszCmdLineParms == NULL )
	{
		write_err_msg( "error allocating memory for pInputOpts->pszCmdLineParms\n");
		goto cleanup;
	}
	
	memset( pInputOpts->pszCmdLineParms, 0, lenCmdLineParms + 1 );
	for ( i=1; i<argc; i++ )
	{
		if ( i > 1 )
			strcat( pInputOpts->pszCmdLineParms, " ");
		if ( parmValNeedsQuotes( argv[i] ) )
		{
			strcat( pInputOpts->pszCmdLineParms, "\"");
			strcat( pInputOpts->pszCmdLineParms, argv[i] );
			strcat( pInputOpts->pszCmdLineParms, "\"");
		}
		else
			strcat( pInputOpts->pszCmdLineParms, argv[i] );
	}
	

	while ((c = getopt_long(argc, argv, "abcCdDf:F:h:in:oOp:RsS:t:U:vWxX:Z:",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				dataOnly = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'b':			/* Dump blobs */
				bOutputBlobs = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'c':			/* clean (i.e., drop) schema prior to
								 * create */
				outputClean = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'C':			/* Create DB */
				outputCreate = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'd':			/* dump data as proper insert strings */
				dumpData = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'D':			/* dump data as proper insert strings with
								 * attr names */
				dumpData = true;
				attrNames = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

/* Invalid for mpp_dump.
			case 'f':
				iopt->filename = optarg;
				break;
*/
			case 'F':
				pszFormat = optarg;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;

			case 'h':			/* server host */
				pInputOpts->pszPGHost = Safe_strdup(optarg);
				break;

			case 'i':			/* ignore database version mismatch */
				bIgnoreVersion = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'n':			/* Dump data for this schema only */
				selectSchemaName = Safe_strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;

			case 'o':			/* Dump oids */
				bOids = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'O':			/* Don't reconnect to match owner */
				outputNoOwner = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'p':			/* server port */
				pInputOpts->pszPGPort = Safe_strdup(optarg);
				break;

			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;
				
			case 's':			/* dump schema only */
				schemaOnly = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

/* Invalid for mpp_dump.
			case 'S':			
				pInputOpts->pszOutputSuperuser = strdup(optarg);
				break;
*/				
			case 't':			/* Dump data for this table only */
				selectTableName = Safe_strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;

			case 'u':
				bForcePassword = true;
				pInputOpts->pszUserName = simple_prompt("User name: ", 100, true);
				break;

			case 'U':
				pInputOpts->pszUserName = Safe_strdup(optarg);
				break;

			case 'v':			/* verbose */
				g_verbose = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'W':
				bForcePassword = true;
				break;

			case 'x':			/* skip ACL dump */
				aclsSkip = true;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

				/*
				 * Option letters were getting scarce, so I invented this
				 * new scheme: '-X feature' turns on some feature. Compare
				 * to the -f option in GCC.  You should also add an
				 * equivalent GNU-style option --feature.  Features that
				 * require arguments should use '-X feature=foo'.
				 */
			case 'X':
				if (strcmp(optarg, "use-set-session-authorization") == 0)
					/* no-op, still allowed for compatibility */ ;
				else if (strcmp(optarg, "disable-triggers") == 0)
				{
					disable_triggers = 1;
					pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				}
				else
				{
					fprintf(stderr,
							("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, ("Try \"%s --help\" for more information.\n"), progname);
					goto cleanup;
				}
				break;

			case 'Z':			/* Compression Level */
				compressLevel = atoi(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;
				/* This covers the long options equivalent to -X xxx. */

			case 0:
				break;
			
			case 1:
				/* mpp-c remote compression program */
				pInputOpts->pszCompressionProgram = Safe_strdup(optarg);
				break;

			case 2:
				/* mpp-cf control file */
                 /* temporary disabled
				if ( bSawCDB_S_Option )
				{
					// Ignore mpp_s option
					write_err_msg( "ignoring the mpp-s option, since the mpp-cf option is set.\n");
				}
				
				if ( !FillCDBSet( &pInputOpts->set, optarg ) )
					goto cleanup;
				
				// Validate that none of the instid's are hosts
				if ( 0 < CountInstanceHosts( &pInputOpts->set ) )
				{
					write_err_msg( "The control file %s contains mpp instances specifies as host name or ip address.  This is not allowed.\n",
									optarg);
					goto cleanup;
				}
                 */

				break;
				
			case 3:
				/* mpp-d backup remote directory */
				pInputOpts->pszBackupDirectory = Safe_strdup(optarg);
				break;
				
			case 4:
				/* mpp-r report directory */
				pInputOpts->pszReportDirectory = Safe_strdup(optarg);
				break;
			case 5:
				/* mpp-s backup set specification */
                 /* temporary disabled
				bSawCDB_S_Option = true;
				if ( pInputOpts->set.type == SET_INDIVIDUAL )
				{
					// Ignore this, as we've already encountered an individual spec
					write_err_msg( "ignoring the mpp-s option, since the mpp-cf option is set.\n");
				}
				else
				{
					if ( strcasecmp( optarg, "a") == 0 ) 
						pInputOpts->set.type = SET_ALL;
					else if ( strcasecmp( optarg, "t") == 0 )
						pInputOpts->set.type = SET_TAILS_ONLY;
					else if ( strcasecmp( optarg, "h") == 0 )
						pInputOpts->set.type = SET_HEAD_ONLY;
					else
					{
						write_err_msg( "invalid mpp-s option %s.  Must be a, t, or h.\n", optarg);
						goto cleanup;
					}
				}
                 */
				break;
				
			default:
				fprintf(stderr, ("Try \"%s --help\" for more information.\n"), progname);
				goto cleanup;
		}
	}

	if ( optind < argc - 1 )
	{
		fprintf(stderr, ("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind + 1]);
		fprintf(stderr, ("Try \"%s --help\" for more information.\n"),
				progname);
		goto cleanup;
	}

	/* Get database name from command line */
    /* do not care dbname, always connecting to template1.
     * 
	if (optind < argc)
		pInputOpts->pszDBName = Safe_strdup(argv[optind]);
    */
    
    pInputOpts->pszDBName = Safe_strdup("template1");
	
	if (dataOnly && schemaOnly)
	{
		write_err_msg( "options \"schema only\" (-s) and \"data only\" (-a) cannot be used together\n");
		goto cleanup;
	}

	if (dataOnly && outputClean)
	{
		write_err_msg( "options \"clean\" (-c) and \"data only\" (-a) cannot be used together\n");
		goto cleanup;
	}

	if (bOutputBlobs && selectTableName != NULL)
	{
		write_err_msg( "large-object output not supported for a single table\n");
		write_err_msg( "use a full dump instead\n");
		goto cleanup;
	}

	if (bOutputBlobs && selectSchemaName != NULL)
	{
		write_err_msg( "large-object output not supported for a single schema\n");
		write_err_msg( "use a full dump instead\n");
		goto cleanup;
	}

	if ( dumpData && bOids )
	{
		write_err_msg( "INSERT (-d, -D) and OID (-o) options cannot be used together\n");
		write_err_msg( "(The INSERT command cannot set OIDs.)\n");
		goto cleanup;
	}

	if (bOutputBlobs && (pszFormat[0] == 'p' || pszFormat[0] == 'P'))
	{
		write_err_msg( "large-object output is not supported for plain-text dump files\n");
		write_err_msg( "(Use a different output format.)\n");
		goto cleanup;
	}
    
    if (pInputOpts->pszPGPort == NULL)
    {
        if (getenv("PGPORT") != NULL)
            pInputOpts->pszPGPort = Safe_strdup(getenv("PGPORT"));
    }
	
	bRtn = true;
	
cleanup:

	return bRtn;
}

/*
 * freeThreadParmArray: This function frees the memory allocated for the pData element of the 
 * ThreadParmArray, as well as the strings allocated within each element 
 * of the array.  It does not free the pParmAr itself.  
 */
void freeThreadParmArray( ThreadParmArray* pParmAr )
{
	int i;
	for ( i=0; i<pParmAr->count; i++ )
	{
		ThreadParm* p = &pParmAr->pData[i];
		
		if ( p->pszRemoteBackupPath != NULL )
			free( p->pszRemoteBackupPath );
		
		if ( p->pszErrorMsg != NULL )
			free( p->pszErrorMsg );
	}
	
	if ( pParmAr->pData != NULL )
		free( pParmAr->pData );
	
	pParmAr->count = 0;
	pParmAr->pData = NULL;
}

/*
 * help: This function prints the usage info to stdout. 
 */
void help(const char *pszProgName)
{
	printf(("%s dumps a database as a text file or to other formats.\n\n"), pszProgName);
	printf(("Usage:\n"));
	printf(("  %s [OPTION]... [DBNAME]\n"), pszProgName);

	printf(("\nGeneral options:\n"));
	printf(("  -f, --file=FILENAME      output file name\n"));
	printf(("  -F, --format=c|t|p       output file format (custom, tar, plain text)\n"));
	printf(("  -i, --ignore-version     proceed even when server version mismatches\n"
			 "                           pg_dump version\n"));
	printf(("  -v, --verbose            verbose mode\n"));
	printf(("  -Z, --compress=0-9       compression level for compressed formats\n"));
	printf(("  --help                   show this help, then exit\n"));
	printf(("  --version                output version information, then exit\n"));

	printf(("\nOptions controlling the output content:\n"));
	printf(("  -a, --data-only          dump only the data, not the schema\n"));
	printf(("  -b, --blobs              include large objects in dump\n"));
	printf(("  -c, --clean              clean (drop) schema prior to create\n"));
	printf(("  -C, --create             include commands to create database in dump\n"));
	printf(("  -d, --inserts            dump data as INSERT, rather than COPY, commands\n"));
	printf(("  -D, --column-inserts     dump data as INSERT commands with column names\n"));
	printf(("  -e, --comprerssion-program  compression program for remote inline output compression\n"));
	printf(("  -n, --schema=SCHEMA      dump the named schema only\n"));
	printf(("  -o, --oids               include OIDs in dump\n"));
	printf(("  -O, --no-owner           do not output commands to set object ownership\n"
			 "                           in plain text format\n"));
	printf(("  -r, --backup-directoru   remote backup directory\n"));
	printf(("  -s, --schema-only        dump only the schema, no data\n"));
	printf(("  -S, --superuser=NAME     specify the superuser user name to use in\n"
			 "                           plain text format\n"));
	printf(("  -t, --table=TABLE        dump the named table only\n"));
	printf(("  -x, --no-privileges      do not dump privileges (grant/revoke)\n"));
	printf(("  -X disable-triggers, --disable-triggers\n"
			 "                           disable triggers during data-only restore\n"));

	printf(("\nConnection options:\n"));
	printf(("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(("  -p, --port=PORT          database server port number\n"));
	printf(("  -U, --username=NAME      connect as specified database user\n"));
	printf(("  -W, --password           force password prompt (should happen automatically)\n"));
	
	printf(("\nCDB specific options:\n"));
	printf(("  --mpp-c=UTILITY          compression utility name for in-line compression\n"));
	//printf(("  --mpp-cf=CONTROLFILEPATH path to control file with backup set info\n"));
	printf(("  --mpp-d=BACKUPFILEDIR    directory where backup files are placed\n"));
	printf(("  --mpp-r=REPORTFILEDIR    directory where report file is placed\n"));
	//printf(("  --mpp-s=a|t|h            backup set indicator (all, tails only, or head only)\n"));

	printf(("\nIf no database name is supplied, then the PGDATABASE environment\n"
			 "variable value is used.\n\n"));
	printf(("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}

/*
 * installSignalHandlers: This function sets both the SIGINT and SIGTERM signal handlers
 * to the routine myHandler. 
 */
void installSignalHandlers(void)
{
	struct sigaction act, oact;
	
	act.sa_handler = myHandler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_flags |= SA_RESTART;

	// Install SIGINT interrupt handler						
	if ( sigaction(SIGINT, &act, &oact) < 0 )
	{
		write_err_msg("Error trying to set SIGINT interrupt handler\n");
	}

	act.sa_handler = myHandler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_flags |= SA_RESTART;

	// Install SIGTERM interrupt handler						
	if ( sigaction(SIGTERM, &act, &oact) < 0 )
	{
		write_err_msg("Error trying to set SIGTERM interrupt handler\n");
	}
}


/*
 * myHandler: This function is the signal handler for both SIGINT and SIGTERM signals.  
 * It simply sets a global variable, which is checked by each thread to see whether it should cancel
 * the backup running on the remote database.
 */
void myHandler(SIGNAL_ARGS)
{
	static bool bAlreadyHere = false;
	
	if ( bAlreadyHere )
	{
		return;
	}
	
	bAlreadyHere = true;

	g_b_SendCancelMessage = true;
}

/*
 * parmValNeedsQuotes: This function checks to see whether there is any whitespace in the parameter value.  
 * This is used for pass thru parameters, top know whether to enclose them in quotes or not.
 */
bool parmValNeedsQuotes( const char* Value )
{
	static regex_t rFinder;
	static bool bCompiled			= false;
	static bool bAlwaysPutQuotes	= false;
	bool bPutQuotes = false;
	
	if ( !bCompiled )
	{
		if ( 0 != regcomp( &rFinder, "[[:space:]]", REG_EXTENDED|REG_NOSUB ) )
			bAlwaysPutQuotes = true;
		
		bCompiled = true;
	}

	if ( !bAlwaysPutQuotes )
	{
		if ( regexec( &rFinder, Value, 0, NULL, 0 ) == 0 )
			bPutQuotes = true;
	}
	else
		bPutQuotes = true;
	
	return bPutQuotes;
}

/*
 * reportBackupResults: This function outputs a summary of the backup results to a file or to stdout
 */
int reportBackupResults( const char* pszReportDirectory, const ThreadParmArray* pParmAr )
{
	FILE* fRptFile = NULL;
	FILE* fRpt = NULL;
	char* pszReportPathName = NULL;
	char* pszFormat;
	ThreadParm* pParm0;
	int i;
    int failCount;
	char* pszStatus;
	char* pszMsg;
	
	if ( pParmAr == NULL || pParmAr->count == 0 )
	{
		write_err_msg( "No results to report." );
		return 0;
	}
	
	if ( pszReportDirectory == NULL || *pszReportDirectory == '\0' )
		fRpt = stdout;
	else
	{
		/* See whether we can create the file in the report directory */
		if ( pszReportDirectory[ strlen(pszReportDirectory) - 1] != '/'  )
			pszFormat = "%s/mpp_dump_%s.rpt";
		else
			pszFormat = "%smpp_dump_%s.rpt";
		
		pszReportPathName = MakeString( pszFormat, pszReportDirectory, pParmAr->pData[0].pOptionsData->pszKey );
		
		fRptFile = fopen( pszReportPathName, "w" );
		if ( fRptFile == NULL )
		{
			write_err_msg( "Cannot open report file %s for writing.  Will use stdout instead.\n",
						pszReportPathName );
			fRpt = stdout;
		}
		else
		{
			fRpt = fRptFile;
		}
	}
	
	/* Write to FILE a report showing the timestamp key, what the command line options were,
	 * .which segments/instance pairs were backed up, to where, and any errors that occurred.
	 */
	pParm0 = &pParmAr->pData[0];
	
	fprintf( fRpt, "CDB Backup Report\n" );
	fprintf( fRpt, "Timestamp Key: %s\n", pParm0->pOptionsData->pszKey );

	fprintf( fRpt, "mpp_dump Command Line: %s\n",  
			StringNotNull( pParm0->pOptionsData->pszCmdLineParms, "None" ) );

	fprintf( fRpt, "Pass through Command Line Options: %s\n",  
			StringNotNull( pParm0->pOptionsData->pszPassThroughParms, "None" ) );
	
	fprintf( fRpt, "Compression Program: %s\n", 
			StringNotNull( pParm0->pOptionsData->pszCompressionProgram, "None" ) );
	
	fprintf( fRpt, "\n" );
	fprintf( fRpt, "Individual Results\n" );
	
    failCount=0;
	for ( i=0; i<pParmAr->count; i++ )
	{
		ThreadParm* pParm = &pParmAr->pData[i];
		
        if ( !pParm->bSuccess )
            failCount++;
		pszStatus = pParm->bSuccess ? "Succeeded" : "Failed with error: \n{";
		pszMsg = pParm->pszErrorMsg;
		if ( pszMsg == NULL )
			pszMsg = "";
		
		fprintf( fRpt, "\tInstance %d Segment %d Host %s Port %d Database %s BackupFile %s: %s %s%s\n",  
				pParm->pSegInstData->instid, 
				pParm->pSegInstData->segid, 
				StringNotNull( pParm->pSegInstData->pszHost, "localhost"),
				pParm->pSegInstData->port,
				pParm->pSegInstData->pszDBName,
				StringNotNull( pParm->pszRemoteBackupPath, ""),
				pszStatus,
				pszMsg,
                 (strcmp(pszMsg,"")==0)?"":"}"
				);
	}
    
    if ( failCount == 0 )
        fprintf( fRpt, "\n%s  utility finished successfully.\n", progname );
    else
        fprintf( fRpt, "\n%s  utility finished unsuccessfully with  %d  failures.\n", progname, failCount );
        
	if ( fRptFile != NULL )
	{
		fclose( fRptFile );
		write_err_msg( "Report results written to %s.\n", pszReportPathName );
        if ( failCount == 0 )
            write_err_msg("\n%s  utility finished successfully.\n", progname );
        else
            write_err_msg("\n%s  utility finished unsuccessfully with  %d  failures.\n", progname, failCount );
	}
	
	if ( pszReportPathName != NULL )
		free( pszReportPathName );
	
    return failCount;
}

/*
 * spinOffThreads: This function deals with all the threads that drive the backend backups.
 * First, it creates the timestamp key, initializes the ThreadParmArray, and 
 * the thread synchronization objects.  Then it loops and creates each of the threads.  
 * It waits on a condition variable.  This will release when all threads have
 * attained the SET_SERIALIZABLE state. (or have failed to do so.)
 * It then commits the transaction, which releases the locks it holds on all the mpp managed tables.
 * IOt then waits for all threads to finish. 
 */
void spinOffThreads(PGconn* pConn, InputOptions* pInputOpts, const SegmentInstanceArray* pSegInstAr, ThreadParmArray* pParmAr )
{
	int i;
	ThreadParm* pParm;
	pthread_cond_init( &MyCondVar, NULL );
	pthread_mutex_init( &MyMutex, NULL );
	
	pInputOpts->pszKey = GenerateTimestampKey();
	
	/* Create a thread and have it work on executing the dump on the
	* appropriate tail.
	*/
	if ( !createThreadParmArray( pSegInstAr->count, pParmAr ) )
	{
		write_err_msg( "Cannot allocate memory for thread parameters\n" );
		exit(-1);
	}
	
	nThreadsFinishedLaunch = pParmAr->count;

	write_err_msg( "About to spin off %d threads with timestamp key %s\n",
				nThreadsFinishedLaunch, pInputOpts->pszKey );
	for (i=0; i<pParmAr->count; i++)
	{
		pParm = &pParmAr->pData[i];
		
		pParm->pSegInstData = &pSegInstAr->pData[i];
		pParm->pOptionsData = pInputOpts;
		pParm->bSuccess		= false;
        
        if (!pParm->pSegInstData->isValid)
        {
            decrementFinishedLaunchCount();
            continue;
        }
		
		write_err_msg( "Creating thread to backup segment %d instance %d: host %s port %d database %s\n",
				pParm->pSegInstData->segid,
				pParm->pSegInstData->instid,
				StringNotNull(pParm->pSegInstData->pszHost, "localhost"),
				pParm->pSegInstData->port,
				pParm->pSegInstData->pszDBName
				);
				
		pthread_create(&pParm->thread, 
						NULL, 
						threadProc, 
						pParm);
	}

	/* wait for all threads to complete launch and attain the SET_SERIALIZABLE state. */
	write_err_msg( "%s : Waiting for signal that all remote %s processes have started "
				"their transacactions in serializable isolation level\n",
				GetTimeNow(), pszAgent );

	pthread_mutex_lock( &MyMutex );
	if ( nThreadsFinishedLaunch > 0 )
	{
		pthread_cond_wait( &MyCondVar, &MyMutex );
	}
	pthread_mutex_unlock( &MyMutex );
	
	write_err_msg( "%s: Received signal that all remote %s processes have passed the step "
				"of setting transacactions in serializable isolation level\n",
				GetTimeNow(), pszAgent );

	/* Now commit the transaction so locks will be released */	
	write_err_msg( "Committing transaction on the head instance, thereby releasing locks.\n");
	execCommit( pConn );
	
	write_err_msg( "%s: Waiting for all remote %s programs to finish.\n",
				GetTimeNow(), pszAgent );
				
	/* Wait for all threads to complete */
	for (i=0; i<pParmAr->count; i++)
	{
		ThreadParm* pParm = &pParmAr->pData[i];
        if (pParm->thread == (pthread_t)NULL)
            continue;
		pthread_join( pParm->thread, NULL);
		pParm->thread = (pthread_t)NULL;
	}
}

/* threadProc: This function is used to connect to one database that needs to be backed up,
 * make a mpp_backup_launch call to cause the backup process to begin on the instance hosting the
 * database.  Then it waits for notifications from the process.  It receives a notification
 * wnenever the mpp_dump_agent process writes a row into the mpp_backup_status table.  This happens
 * when the process starts, when it attains the SET_SERIALIZABLE state, and when it finishes.
 */
void* threadProc(void *arg)
{
	bool bDidDecrement = false;
	
	/* The argument is a pointer to a ThreadParm structure 
	 * that stays around for the entire time the program is running.
	 * so we need not worry about making a copy of it.
	 */
	ThreadParm* pParm = (ThreadParm *)arg;
	
	const SegmentInstance* pSegInst	= pParm->pSegInstData;
	const InputOptions* pInputOpts	= pParm->pOptionsData;
	const char* pszKey				= pInputOpts->pszKey;
	PGconn* pConn;
	
	char* pszPassThroughCredentials;
	PQExpBuffer Qry;
	PGresult* pRes; 
	int sock;
	fd_set input_mask;
	bool bSentCancelMessage;
	BackupStateMachine* pState;
	struct timeval tv;
	PGnotify* pNotify;

	/* Block SIGINT and SIGKILL signals (we can handle them in the main thread) */
	sigset_t newset;
	sigemptyset( &newset );
	sigaddset( &newset, SIGINT );
	sigaddset( &newset, SIGKILL );
	pthread_sigmask( SIG_SETMASK, &newset, NULL );
			
	/* Create a connection for this thread */
	pConn = MakeDBConnection( pSegInst, false );
	if ( PQstatus(pConn) == CONNECTION_BAD )
	{
		g_b_SendCancelMessage = true;
		pParm->pszErrorMsg = MakeString( "Connection to segment %d instance %d on host %s failed: %s",
										pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost"), PQerrorMessage(pConn) );
		write_err_msg( pParm->pszErrorMsg );
		PQfinish(pConn);
		decrementFinishedLaunchCount();
		return NULL;
	}
	
	
	/* issue a LISTEN command for 4 different names*/ 
	DoCancelNotifyListen( pConn, true, pszKey, pSegInst->instid, pSegInst->segid, SUFFIX_START );
	DoCancelNotifyListen( pConn, true, pszKey, pSegInst->instid, pSegInst->segid, SUFFIX_SET_SERIALIZABLE );
	DoCancelNotifyListen( pConn, true, pszKey, pSegInst->instid, pSegInst->segid, SUFFIX_SUCCEED );
	DoCancelNotifyListen( pConn, true, pszKey, pSegInst->instid, pSegInst->segid, SUFFIX_FAIL );
	
	/* Execute mpp_backup_launch  */
	
	/*
	 * If there is a password associated with this login, we pass it as a base64 encoded string
	 * in the parameter pszPassThroughCredentials
	 */
	pszPassThroughCredentials = NULL;
	if ( pSegInst->pszDBPswd != NULL && *pSegInst->pszDBPswd != '\0' )
		pszPassThroughCredentials = DataToBase64( pSegInst->pszDBPswd, strlen(pSegInst->pszDBPswd) );
	
	Qry = createPQExpBuffer();
	appendPQExpBuffer( Qry, "SELECT * FROM mpp_backup_launch('%s', '%s', '%s', '%s', '%s')", 
					StringNotNull( pInputOpts->pszBackupDirectory, ""), 
					pszKey, 
					StringNotNull( pInputOpts->pszCompressionProgram, ""), 
					StringNotNull( pInputOpts->pszPassThroughParms, ""),
					StringNotNull( pszPassThroughCredentials, "")
					);
					
	if ( pszPassThroughCredentials != NULL )
		free( pszPassThroughCredentials );
 					
	pRes = PQexec(pConn, Qry->data ); 
	if (!pRes || PQresultStatus(pRes) != PGRES_TUPLES_OK || PQntuples(pRes) == 0 )
	{
		g_b_SendCancelMessage = true;
		pParm->pszErrorMsg = MakeString( "could not start mpp backup: %s", PQerrorMessage(pConn) );
		write_err_msg(pParm->pszErrorMsg);
		PQfinish( pConn );
		decrementFinishedLaunchCount();
		bDidDecrement = true;
		return NULL;
	}
	
	pParm->pszRemoteBackupPath = strdup( PQgetvalue(pRes, 0, 0) );
	
	PQclear(pRes);
	destroyPQExpBuffer(Qry);
	
	/* Now wait for notifications from the back end back end */
	sock = PQsocket( pConn );
	FD_ZERO( &input_mask );
	FD_SET( sock, &input_mask );
	
	bSentCancelMessage = false;
	
	pState = CreateBackupStateMachine( pszKey, pSegInst->instid, pSegInst->segid );
	if ( pState == NULL )
	{
		g_b_SendCancelMessage = true;
		pParm->pszErrorMsg = "error allocating memory for mpp backup";
		write_err_msg(pParm->pszErrorMsg);
		PQfinish( pConn );
		decrementFinishedLaunchCount();
		bDidDecrement = true;
		return NULL;
	}
	
	/* This is a bit involved.  We are waiting for 3 messages: 
	 * one that the backend process has started, 
	 * one that the SET_SERIALIZABLE has happened, 
	 * and the third that FINISH has happened.
	 * To avoid polling, we use the NOTIFY/LISTEM approach.
	 * But we onlt select for 2 secs at a time, so we can see whether or not we need to NOTIFY
	 * our backend to cancel, based on another thread failing.
	 * A BackupStateMachine object is used to manage receiving these notifications
	 */

	while ( !IsFinalState( pState ) )
	{
		// Check to see whether another thread has failed and therefore we should cancel
		if ( g_b_SendCancelMessage && !bSentCancelMessage && HasStarted( pState ) )
		{
			write_err_msg( "noticed that a cancel order is in effect. Informing segment %d instance %d on host %s by notifying on connection\n", 
							pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
			// Either one of the other threads have failed, or a Ctrl C was received.  So post a cancel message
//			InsertStatusRow(pConn, pszKey, pSegInst->instid, pSegInst->segid, TASK_CANCEL, TASK_RC_SUCCESS, "Cancel Requested");
			DoCancelNotifyListen( pConn, false, pszKey, pSegInst->instid, pSegInst->segid, NULL );
			bSentCancelMessage = true;
		}
		
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		
		if ( select( sock+1, &input_mask, NULL, NULL, &tv ) < 0 )
		{
			g_b_SendCancelMessage = true;
			pParm->pszErrorMsg = MakeString("select failed for backup key %s, instid %d, segid %d failed\n",
											pszKey, pSegInst->instid, pSegInst->segid);
			write_err_msg( pParm->pszErrorMsg );
			PQfinish(pConn);
			DestroyBackupStateMachine(pState);
			if ( !bDidDecrement )
			{
				decrementFinishedLaunchCount();
				bDidDecrement = true;
			}

			return NULL;
		}
		
		// See whether the connection went down
		if ( PQstatus( pConn ) == CONNECTION_BAD )
		{
			g_b_SendCancelMessage = true;
			pParm->pszErrorMsg = MakeString("connection went down for backup key %s, instid %d, segid %d\n",
											pszKey, pSegInst->instid, pSegInst->segid);
			write_err_msg( pParm->pszErrorMsg );
			PQfinish(pConn);
			DestroyBackupStateMachine(pState); 
			if ( !bDidDecrement )
			{
				decrementFinishedLaunchCount();
				bDidDecrement = true;
			}
			return NULL;
		}
		
		PQconsumeInput( pConn );

		CleanupNotifications( pState );

		while ( NULL != (pNotify = PQnotifies(pConn)))
		{
			if ( strncasecmp( pState->pszNotifyRelName, pNotify->relname, 
								strlen(pState->pszNotifyRelName) ) == 0 )
			{
				if ( !AddNotificationtoBackupStateMachine( pState, pNotify) )
				{
					g_b_SendCancelMessage = true;
					pParm->pszErrorMsg = MakeString("error allocating memory for mpp backup\n");
					write_err_msg( pParm->pszErrorMsg );
					PQfinish(pConn);
					DestroyBackupStateMachine(pState);
					if ( !bDidDecrement )
					{
						decrementFinishedLaunchCount();
						bDidDecrement = true;
					}
					return NULL;
				}
			}
		}

		ProcessInput( pState );
		if ( !bDidDecrement && HasReceivedSetSerializable( pState ) )
		{
			decrementFinishedLaunchCount();
			bDidDecrement = true;
		}
	}
	
	if ( !bDidDecrement )
	{
		decrementFinishedLaunchCount();
		bDidDecrement = true;
	}

	/* We don't get here unless the BackupStateMachine reached a final state.
	 * If the status indicates ar error, we process this error in the switch statement below.
	 */
	if ( !pState->bStatus )
	{
		g_b_SendCancelMessage = true;
		pParm->bSuccess = false;
		write_err_msg("backup failed for segment %d instance %d on host %s\n", 
					pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
		
		switch( pState->currentState )
		{
			case STATE_TIMEOUT:
//				InsertStatusRow(pConn, pszKey, pSegInst->instid, pSegInst->segid, TASK_CANCEL, TASK_RC_SUCCESS, "Cancel Requested");
				DoCancelNotifyListen( pConn, false, pszKey, pSegInst->instid, pSegInst->segid, NULL );
				bSentCancelMessage = true;

				pParm->pszErrorMsg = MakeString( "backup failed to set transaction level serializable for segment %d instance %d on host %s in the required time interval\n", 
												pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
				break;												
			case STATE_BACKUP_ERROR:
//				if ( pState->pszBackupError != NULL )
//					pParm->pszErrorMsg = strdup( pState->pszBackupError );
//				else
//					pParm->pszErrorMsg = MakeString("backup failed for segment %d instance %d on host %s\n", 
//										pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost"));
				// Make call to get error message from file on server
				pParm->pszErrorMsg = ReadBackendBackupFile( pConn, pInputOpts->pszBackupDirectory, pszKey, BFT_BACKUP_STATUS );
					
				break;												

			case STATE_UNEXPECTED_INPUT:
//				InsertStatusRow(pConn, pszKey, pSegInst->instid, pSegInst->segid, TASK_CANCEL, TASK_RC_SUCCESS, "Cancel Requested");
				DoCancelNotifyListen( pConn, false, pszKey, pSegInst->instid, pSegInst->segid, NULL );
				bSentCancelMessage = true;

				pParm->pszErrorMsg = MakeString("Unexpected task id encountered while performing backup for for segment %d instance %d on host %s",
												pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
				break;

			default:
				break;
		}

		write_err_msg( pParm->pszErrorMsg );
	}
	else
	{
		pParm->bSuccess = true;
		write_err_msg("backup succeeded for segment %d instance %d on host %s\n", 
					pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
	}

	PQfinish(pConn);
	DestroyBackupStateMachine(pState); 
	
	return(NULL);
}

static void getDatabaseConnections(PGconn *conn, PGconn ***conn_dbs, int *numberOfDbs, InputOptions *inputOpts)
{
    PGresult   *res;
    int         i;

    res = executeQuery(conn, "SELECT datname FROM pg_database WHERE datallowconn");
    *numberOfDbs = PQntuples(res);
    *conn_dbs = (PGconn **)malloc(sizeof(PGconn *) * *numberOfDbs);

    for (i = 0; i < *numberOfDbs; i++)
    {
        (*conn_dbs)[i] = MakeConnectionToDatabase(PQgetvalue(res,i,0), inputOpts->pszPGHost, 
                                    inputOpts->pszPGPort, inputOpts->pszUserName, 
                                    bForcePassword, bIgnoreVersion, false);
    }


    PQclear(res);
}

/*
 * Run a query, return the results, exit program on failure.
 */
static PGresult *
executeQuery(PGconn *conn, const char *query)
{
    PGresult   *res;

    if (g_verbose)
        write_err_msg("%s: executing %s\n", progname, query);

    res = PQexec(conn, query);
    if (!res ||
        PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        write_err_msg("%s: query failed: %s", progname, PQerrorMessage(conn));
        write_err_msg("%s: query was: %s\n", progname, query);
        PQfinish(conn);
        exit(1);
    }

    return res;
}



