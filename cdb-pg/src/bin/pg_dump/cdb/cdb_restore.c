/*-------------------------------------------------------------------------
 *
 * cdb_restore.c
 *	  cdb_restore is a utility for restoring a cdb cluster of postgres databases
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <regex.h>
#include <ctype.h>
#include <pthread.h>
#include "getopt_long.h"
#include "pqexpbuffer.h"
#include "libpq-fe.h"
#include "fe-auth.h"
#include "../pg_backup.h"
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


/* Forward decls */
static void freeThreadParmArray( ThreadParmArray* pParmAr );
static bool createThreadParmArray( int nCount, ThreadParmArray* pParmAr);
static char* addPassThroughParm( char Parm, const char* pszValue, char* pszPassThroughParmString );
static void	fillInputOptions(int argc, char** argv, InputOptions* pInputOpts);
static bool parmValNeedsQuotes( const char* Value );
static void usage();
static void* threadProc(void *arg);
static bool restoreHead( InputOptions* pInputOpts, PGconn *pConn, SegmentInstance *pSegInst, ThreadParm* pParm );
static void spinOffThreads(PGconn* pConn, InputOptions* pInputOpts, const SegmentInstanceArray* pSegInstAr, ThreadParmArray* pParmAr );
static int reportRestoreResults( const char* pszReportDirectory, const ThreadParm* pHeadParm, const ThreadParmArray* pParmAr );

static char* progname;
static bool g_b_SendCancelMessage = false;
typedef struct option optType;

static RestoreOptions *opts = NULL;

static bool bForcePassword = false;
static bool bIgnoreVersion = false;
static const char* pszAgent = "cdb_restore_agent";

int
main(int argc, char **argv)
{
	PGconn* pConn = NULL;
    int failCount=-1;
	char szErrortmp[PQERRORMSG_LENGTH];
    int i;
    SegmentInstance *segAr;
	
	/* This struct holds the values of the command line parameters */
	InputOptions			inputOpts;
	/* This struct holds an array of the segment instance pairs from the head cdb tables */
	SegmentInstanceArray	segInstAr;
	/* This struct holds an array of the thread parameters */
	ThreadParmArray			parmAr;
	ThreadParm				headParm;
	
	memset( &inputOpts, 0, sizeof(inputOpts) );
	memset( &segInstAr, 0, sizeof(segInstAr) );
	memset( &parmAr, 0, sizeof(parmAr) );
	memset( &headParm, 0, sizeof(headParm) );
	
	/* Parse command line for options */
	write_err_msg("Analyzing command line options.\n" );
	fillInputOptions( argc, argv, &inputOpts );

	/* Connect to database on the head*/
	write_err_msg( "Connecting to head instance on host %s port %s database %s.\n",
				StringNotNull( inputOpts.pszPGHost, "localhost"), 
				StringNotNull( inputOpts.pszPGPort, "5432"),
				StringNotNull( inputOpts.pszHeadDBName, fe_getauthname(szErrortmp) ) );
						
	pConn = MakeConnectionToDatabase(inputOpts.pszHeadDBName, inputOpts.pszPGHost, 
											inputOpts.pszPGPort, inputOpts.pszUserName, 
											bForcePassword, bIgnoreVersion, true);
	if ( pConn == NULL )
		goto cleanup;

	/* Read cdb segment/instance configuration from the head */
	write_err_msg( "Reading cdb configuration info from head instance database.\n");
	if ( !GetSegmentInstanceArray(pConn, inputOpts.pszHeadDBName, &inputOpts.set, &segInstAr, true) )
		goto cleanup;
    
    // find head node
    segAr = NULL;
    for (i=0; i<segInstAr.count; i++)
    {
        segAr = segInstAr.pData+i;
        if (segAr->instid == 1)
            break;
    }
    
    // Is the head part of the restore set?
    if ( FindInCDBSet( &inputOpts.set, 1, segAr->segid ) )
    {
        if ( !restoreHead( &inputOpts, pConn, segAr, &headParm ) )
            goto cleanup;
    }
    
	if ( segInstAr.count > 0 )
	{
		/* Create the threads to talk to each of the databases being restored.
		 * Wait for them to finish.   
		 */ 
		spinOffThreads(pConn, &inputOpts, &segInstAr, &parmAr);
		
		write_err_msg( "%s: All remote %s programs are finished.\n", GetTimeNow(), pszAgent );
	}
	
	/* Produce results report */
	failCount = reportRestoreResults( inputOpts.pszReportDirectory, &headParm, &parmAr );
	
cleanup:	
	FreeInputOptions( &inputOpts );
	if ( opts != NULL )
		free( opts );
	FreeSegmentInstanceArray( &segInstAr );
	freeThreadParmArray( &parmAr );

	if ( headParm.pszErrorMsg )
		free( headParm.pszErrorMsg );
	
	if ( headParm.pszRemoteBackupPath )
		free( headParm.pszRemoteBackupPath );
	
	if ( pConn != NULL )
		PQfinish( pConn );
	
	return failCount;
}

static void
usage()
{
	printf(("%s restores a PostgreSQL database from an archive created by pg_dump.\n\n"), progname);
	printf(("Usage:\n"));
	printf(("  %s [OPTION]... [FILE]\n"), progname);

	printf(("\nGeneral options:\n"));
	printf(("  -d, --dbname=NAME        output database name\n"));
	printf(("  -f, --file=FILENAME      output file name\n"));
	printf(("  -F, --format=c|t         specify backup file format\n"));
	printf(("  -i, --ignore-version     proceed even when server version mismatches\n"));
	printf(("  -l, --list               print summarized TOC of the archive\n"));
	printf(("  -v, --verbose            verbose mode\n"));
	printf(("  --help                   show this help, then exit\n"));
	printf(("  --version                output version information, then exit\n"));

	printf(("\nOptions controlling the output content:\n"));
	printf(("  -a, --data-only          restore only the data, no schema\n"));
	printf(("  -c, --clean              clean (drop) schema prior to create\n"));
	printf(("  -C, --create             issue commands to create the database\n"));
	printf(("  -I, --index=NAME         restore named index\n"));
	printf(("  -L, --use-list=FILENAME  use specified table of contents for ordering\n"
			 "                           output from this file\n"));
	printf(("  -N, --orig-order         restore in original dump order\n"));
	printf(("  -o, --oid-order          restore in OID order\n"));
	printf(("  -O, --no-owner           do not output commands to set object ownership\n"));
	printf(("  -P, --function=NAME(args)\n"
			 "                           restore named function\n"));
	printf(("  -r, --rearrange          rearrange output to put indexes etc. at end\n"));
	printf(("  -s, --schema-only        restore only the schema, no data\n"));
	printf(("  -S, --superuser=NAME     specify the superuser user name to use for\n"
			 "                           disabling triggers\n"));
	printf(("  -t, --table=NAME         restore named table\n"));
	printf(("  -T, --trigger=NAME       restore named trigger\n"));
	printf(("  -x, --no-privileges      skip restoration of access privileges (grant/revoke)\n"));
	printf(("  -X disable-triggers, --disable-triggers\n"
			 "                           disable triggers during data-only restore\n"));

	printf(("\nConnection options:\n"));
	printf(("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(("  -p, --port=PORT          database server port number\n"));
	printf(("  -U, --username=NAME      connect as specified database user\n"));
	printf(("  -W, --password           force password prompt (should happen automatically)\n"));

	printf(("\nCDB specific options:\n"));
	printf(("  --cdb-c=UTILITY          compression utility name for in-line compression\n"));
	//printf(("  --cdb-cf=CONTROLFILEPATH path to control file with restore set info\n"));
	printf(("  --cdb-d=BACKUPFILEDIR    directory where backup files are located\n"));
	printf(("  --cdb-i                  ignore error\n"));
	//printf(("  --cdb-hdb=HEADDBNAME     name of head database\n"));
	printf(("  --cdb-k=KEY              date time backup key from cdb_backup_driver run\n"));
	printf(("  --cdb-r=REPORTFILEDIR    directory where report file is placed\n"));
	//printf(("  --cdb-s=a|t|h            restore set indicator (all, tails only, or head only)\n"));
	
	printf(("\nIf no input file name is supplied, then standard input is used.\n\n"));
	printf(("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}

void fillInputOptions(int argc, char** argv, InputOptions* pInputOpts)
{
	int			c;
	//Archive    *AH;
	//char	   *inputFileSpec;
	extern int	optind;
	extern char *optarg;
	static int	use_setsessauth = 0;
	static int	disable_triggers = 0;
	bool bSawCDB_S_Option;
	int lenCmdLineParms;
	int i;

	struct option cmdopts[] = {
		{"clean", 0, NULL, 'c'},
		{"create", 0, NULL, 'C'},
		{"data-only", 0, NULL, 'a'},
		{"dbname", 1, NULL, 'd'},
		{"file", 1, NULL, 'f'},
		{"format", 1, NULL, 'F'},
		{"function", 1, NULL, 'P'},
		{"host", 1, NULL, 'h'},
		{"ignore-version", 0, NULL, 'i'},
		{"index", 1, NULL, 'I'},
		{"list", 0, NULL, 'l'},
		{"no-privileges", 0, NULL, 'x'},
		{"no-acl", 0, NULL, 'x'},
		{"no-owner", 0, NULL, 'O'},
		{"no-reconnect", 0, NULL, 'R'},
		{"port", 1, NULL, 'p'},
		{"oid-order", 0, NULL, 'o'},
		{"orig-order", 0, NULL, 'N'},
		{"password", 0, NULL, 'W'},
		{"rearrange", 0, NULL, 'r'},
		{"schema-only", 0, NULL, 's'},
		{"superuser", 1, NULL, 'S'},
		{"table", 1, NULL, 't'},
		{"trigger", 1, NULL, 'T'},
		{"use-list", 1, NULL, 'L'},
		{"username", 1, NULL, 'U'},
		{"verbose", 0, NULL, 'v'},

		/*
		 * the following options don't have an equivalent short option
		 * letter, but are available as '-X long-name'
		 */
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},

		/*
		 * the following are cdb specific, and don't have an equivalent short option
		 */
		{"cdb-c", required_argument, NULL, 1},
		//{"cdb-cf", required_argument, NULL, 2},
		{"cdb-d", required_argument, NULL, 3},
		{"cdb-i", no_argument, NULL, 4},
		//{"cdb-hdb", required_argument, NULL, 5},
		{"cdb-k", required_argument, NULL, 6},
		{"cdb-r", required_argument, NULL, 7},
		{"cdb-s", required_argument, NULL, 8},
		{NULL, 0, NULL, 0}
	};

	/* Initialize option fields  */
	memset( pInputOpts, 0, sizeof(InputOptions) );
	pInputOpts->set.type		= SET_ALL;
	pInputOpts->set.count		= 0;
	pInputOpts->set.pData		= NULL;
	pInputOpts->bOnErrorStop	= true;
	
#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain("pg_dump", LOCALEDIR);
	textdomain("pg_dump");
#endif

	opts = (RestoreOptions *) calloc(1, sizeof(RestoreOptions));
	if ( opts == NULL )
	{
		write_err_msg("error allocating memory for RestoreOptions");
		exit(0);
	}
	
	opts->format = archUnknown;
	opts->suppressDumpWarnings = false;

	progname = (char *)get_progname(argv[0]);
	
	bSawCDB_S_Option = false;

	if (argc == 1)
	{
		usage();
		exit(0);
	}
	
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("cdb_restore (Bizgres MPP) " PG_VERSION);
			exit(0);
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
		exit(1);
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
	
	while ((c = getopt_long(argc, argv, "acCd:f:F:h:iI:lL:NoOp:P:rRsS:t:T:uU:vWxX:",
							cmdopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				opts->dataOnly = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
			case 'c':			/* clean (i.e., drop) schema prior to
								 * create */
				opts->dropSchema = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
			case 'C':
				opts->create = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
			case 'd':
				opts->dbname = strdup(optarg);
				pInputOpts->pszDBName = Safe_strdup(opts->dbname);
                 pInputOpts->pszHeadDBName = Safe_strdup(opts->dbname);
				break;
//			case 'f':			/* output file name */
//				opts->filename = strdup(optarg);
//				break;
//				
			case 'F':
				if (strlen(optarg) != 0)
				{
					opts->formatName = strdup(optarg);
					pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				}

				break;
			case 'h':
				if (strlen(optarg) != 0)
				{
					opts->pghost = strdup(optarg);
					pInputOpts->pszPGHost = Safe_strdup( optarg );
				}
					
				break;
			case 'i':
				opts->ignoreVersion = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

//			case 'l':			/* Dump the TOC summary */
//				opts->tocSummary = 1;
//				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
//				break;

//			case 'L':			/* input TOC summary file name */
//				opts->tocFile = strdup(optarg);
//				break;

			case 'N':
				opts->origOrder = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
			case 'o':
				opts->oidOrder = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
			case 'O':
				opts->noOwner = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
			case 'p':
				if (strlen(optarg) != 0)
				{
					opts->pgport = strdup(optarg);
					pInputOpts->pszPGPort = Safe_strdup(optarg);
				}
				break;
			case 'r':
				opts->rearrange = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;
			case 'P':			/* Function */
				opts->selTypes = 1;
				opts->selFunction = 1;
				opts->functionNames = strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;
			case 'I':			/* Index */
				opts->selTypes = 1;
				opts->selIndex = 1;
				opts->indexNames = strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;
			case 'T':			/* Trigger */
				opts->selTypes = 1;
				opts->selTrigger = 1;
				opts->triggerNames = strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;
			case 's':			/* dump schema only */
				opts->schemaOnly = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;
//			case 'S':			/* Superuser username */
//				if (strlen(optarg) != 0)
//					opts->superuser = strdup(optarg);
//				break;
			case 't':			/* Dump data for this table only */
				opts->selTypes = 1;
				opts->selTable = 1;
				opts->tableNames = strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;

			case 'u':
				opts->requirePassword = true;
				opts->username = simple_prompt("User name: ", 100, true);
				pInputOpts->pszUserName = Safe_strdup(opts->username); 
				break;

			case 'U':
				opts->username = optarg;
				pInputOpts->pszUserName = Safe_strdup(opts->username); 
				break;

			case 'v':			/* verbose */
				opts->verbose = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'W':
				opts->requirePassword = true;
				break;

			case 'x':			/* skip ACL dump */
				opts->aclsSkip = 1;
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, NULL, pInputOpts->pszPassThroughParms ); 
				break;

			case 'X':
				if (strcmp(optarg, "use-set-session-authorization") == 0)
					/* no-op, still allowed for compatibility */ ;
				else if (strcmp(optarg, "disable-triggers") == 0)
					disable_triggers = 1;
				else
				{
					fprintf(stderr,
							("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, ("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				
				pInputOpts->pszPassThroughParms = addPassThroughParm( c, optarg, pInputOpts->pszPassThroughParms ); 
				break;

				/* This covers the long options equivalent to -X xxx. */
			case 0:
				break;

			case 1:
				/* cdb-c remote compression program */
				pInputOpts->pszCompressionProgram = Safe_strdup(optarg);
				break;

			case 2:
				/* cdb-cf control file */
                 /* temporary disabled
				if ( bSawCDB_S_Option )
				{
					// Ignore cdb_s option
					write_err_msg( "ignoring the cdb-s option, since the cdb-cf option is set.\n");
				}
				
				if ( !FillCDBSet( &pInputOpts->set, optarg ) )
					exit(1);
                 */

				break;
				
			case 3:
				/* cdb-d backup remote directory */
				pInputOpts->pszBackupDirectory = Safe_strdup(optarg);
				break;
				
			case 4:
				/* cdb-e on error stop */
				pInputOpts->bOnErrorStop = false;
				break;
				
			case 5:
				/* cdb-hdb head database name */
				//pInputOpts->pszHeadDBName = Safe_strdup(optarg);
				break;
				
			case 6:
				/* cdb-k backup timestamp key */
				pInputOpts->pszKey = Safe_strdup(optarg);
				break;
				
			case 7:
				/* cdb-r report directory */
				pInputOpts->pszReportDirectory = Safe_strdup(optarg);
				break;
			case 8:
				/* cdb-s backup set specification */
                 /* temporay disabled
				bSawCDB_S_Option = true;
				if ( pInputOpts->set.type == SET_INDIVIDUAL )
				{
					// Ignore this, as we've already encountered an individual spec
					write_err_msg( "ignoring the cdb-s option, since the cdb-cf option is set.\n");
				}
				else
                 */
				{
					if ( strcasecmp( optarg, "a") == 0 ) 
						pInputOpts->set.type = SET_ALL;
					else if ( strcasecmp( optarg, "t") == 0 )
						pInputOpts->set.type = SET_TAILS_ONLY;
					else if ( strcasecmp( optarg, "h") == 0 )
						pInputOpts->set.type = SET_HEAD_ONLY;
					else
					{
						write_err_msg( "invalid cdb-s option %s.  Must be a, t, or h.\n", optarg);
						exit(1);
					}
				}
				break;
				
			default:
				fprintf(stderr, ("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

/*	if (optind < argc)
		inputFileSpec = argv[optind];
	else
		inputFileSpec = NULL;
*/
	/* Should get at most one of -d and -f, else user is confused */
	if (opts->dbname == NULL)
	{
		write_err_msg( "Error: %s command line missing the database parameter (-d)\n", progname );
		exit(1);
	}
	opts->useDB = 1;

	if (pInputOpts->pszKey == NULL)
	{
		write_err_msg( "Error: %s command line missing the date time backup key parameter (--cdb-k)\n", progname );
		exit(1);
	}

	if (pInputOpts->pszHeadDBName == NULL)
	{
		write_err_msg( "Error: %s command line missing the head database parameter (--cdb-hdb)\n", progname );
		exit(1);
	}

	if (opts->formatName == NULL)
	{
        /*
		write_err_msg( "Error: %s command line missing the format parameter (-F)\n", progname );
		exit(1);
        */
        opts->formatName = "p";
	}

	opts->disable_triggers = disable_triggers;
	bForcePassword = (opts->requirePassword == 1);
	bIgnoreVersion = (opts->ignoreVersion == 1);
	if (opts->formatName)
	{

		switch (opts->formatName[0])
		{

			case 'c':
			case 'C':
				opts->format = archCustom;
				break;

			case 'f':
			case 'F':
				opts->format = archFiles;
				break;

			case 't':
			case 'T':
				opts->format = archTar;
				break;

			case 'p':
			case 'P':
				opts->format = archNull;
				break;
				
			default:
				write_err_msg("unrecognized archive format '%s'; please specify 'p', 't', or 'c'\n",
						  	opts->formatName);
				exit(1);
		}
	}
    
    if (pInputOpts->pszPGPort == NULL)
    {
        if (getenv("PGPORT") != NULL)
            pInputOpts->pszPGPort = Safe_strdup(getenv("PGPORT"));
    }
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
 * addPassThroughParm: this function adds to the string of pass-through parameters.  These get sent to each
 * backend and get passed to the cdb_dump program.
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

/* threadProc: This function is used to connect to one database that needs to be restored,
 * make a cdb_restore_launch call to cause the restore process to begin on the instance hosting the
 * database.  Then it waits for notifications from the process.  It receives a notification
 * wnenever the cdb_restore_agent process signals with a NOTIFY. This happens when the process starts, 
 * and when it finishes.
 */
void* threadProc(void *arg)
{
	/* The argument is a pointer to a ThreadParm structure 
	 * that stays around for the entire time the program is running.
	 * so we need not worry about making a copy of it.
	 */
	ThreadParm* pParm = (ThreadParm *)arg;
	
	SegmentInstance* pSegInst	= pParm->pSegInstData;
	const InputOptions* pInputOpts	= pParm->pOptionsData;
	const char* pszKey				= pInputOpts->pszKey;
	PGconn* pConn;
	char* pszPassThroughCredentials;
	PQExpBuffer Qry;
	PGresult* pRes; 
	int sock;
	fd_set input_mask;
	bool	bSentCancelMessage;
	bool	bIsFinished;
	bool	bIsStarted;
	int		nTries;
	char* pszNotifyRelName;
	char* pszNotifyRelNameStart;
	char* pszNotifyRelNameSucceed;
	char* pszNotifyRelNameFail;
	struct timeval tv;
	PGnotify* pNotify;
	int nNotifies;
		
	/* Block SIGINT and SIGKILL signals (we can handle them in the main thread) */
	sigset_t newset;
	sigemptyset( &newset );
	sigaddset( &newset, SIGINT );
	sigaddset( &newset, SIGKILL );
	pthread_sigmask( SIG_SETMASK, &newset, NULL );
			
	/* Create a connection for this thread */
	if ( strcasecmp( pInputOpts->pszDBName, pInputOpts->pszHeadDBName ) != 0 )
	{
		if ( pSegInst->pszDBName != NULL )
			free( pSegInst->pszDBName );
		
		pSegInst->pszDBName 	= Safe_strdup( pInputOpts->pszDBName );
	}
	
	pConn = MakeDBConnection( pSegInst, false );
	if ( PQstatus(pConn) == CONNECTION_BAD )
	{
		g_b_SendCancelMessage = true;
		pParm->pszErrorMsg = MakeString( "Connection to segment %d instance %d on host %s failed: %s",
										pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost"), PQerrorMessage(pConn) );
		write_err_msg( pParm->pszErrorMsg );
		PQfinish(pConn);
		return NULL;
	}
	
	/* issue a LISTEN command for 3 different names*/ 
	DoCancelNotifyListen( pConn, true, pszKey, pSegInst->instid, pSegInst->segid, SUFFIX_START );
	DoCancelNotifyListen( pConn, true, pszKey, pSegInst->instid, pSegInst->segid, SUFFIX_SUCCEED );
	DoCancelNotifyListen( pConn, true, pszKey, pSegInst->instid, pSegInst->segid, SUFFIX_FAIL );
	
	/* Execute cdb_restore_launch  */
	
	/*
	 * If there is a password associated with this login, we pass it as a base64 encoded string
	 * in the parameter pszPassThroughCredentials
	 */
	pszPassThroughCredentials = NULL;
	if ( pSegInst->pszDBPswd != NULL && *pSegInst->pszDBPswd != '\0' )
		pszPassThroughCredentials = DataToBase64( pSegInst->pszDBPswd, strlen(pSegInst->pszDBPswd) );
	
	Qry = createPQExpBuffer();
	appendPQExpBuffer( Qry, "SELECT * FROM cdb_restore_launch('%s', '%s', '%s', '%s', '%s', %s)", 
					StringNotNull( pInputOpts->pszBackupDirectory, ""), 
					pszKey, 
					StringNotNull( pInputOpts->pszCompressionProgram, ""), 
					StringNotNull( pInputOpts->pszPassThroughParms, ""),
					StringNotNull( pszPassThroughCredentials, ""),
					pInputOpts->bOnErrorStop ? "true" : "false");
					
	if ( pszPassThroughCredentials != NULL )
		free( pszPassThroughCredentials );
 					
	pRes = PQexec(pConn, Qry->data ); 
	if (!pRes || PQresultStatus(pRes) != PGRES_TUPLES_OK || PQntuples(pRes) == 0 )
	{
		g_b_SendCancelMessage = true;
		pParm->pszErrorMsg = MakeString( "could not start cdb restore: %s", PQerrorMessage(pConn) );
		write_err_msg(pParm->pszErrorMsg);
		PQfinish( pConn );
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
	bIsFinished = false;
	bIsStarted = false;
	nTries = 0;

	pszNotifyRelName = MakeString("N%s_%d_%d",
							pszKey, pSegInst->instid, pSegInst->segid );
	pszNotifyRelNameStart = MakeString("%s_%s", pszNotifyRelName, SUFFIX_START);
	pszNotifyRelNameSucceed = MakeString("%s_%s", pszNotifyRelName, SUFFIX_SUCCEED);
	pszNotifyRelNameFail = MakeString("%s_%s", pszNotifyRelName, SUFFIX_FAIL);
							
	while ( !bIsFinished )
	{
		// Check to see whether another thread has failed and therefore we should cancel
		if ( g_b_SendCancelMessage && !bSentCancelMessage && bIsStarted )
		{
			write_err_msg( "noticed that a cancel order is in effect. Informing segment %d instance %d on host %s\n", 
							pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
			// Either one of the other threads have failed, or a Ctrl C was received.  So post a cancel message
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
			return NULL;
		}
		
		PQconsumeInput( pConn );

		nNotifies = 0;
		while ( NULL != (pNotify = PQnotifies(pConn)))
		{
			if ( strncasecmp( pszNotifyRelName, pNotify->relname, strlen(pszNotifyRelName) ) == 0 )
			{
				nNotifies++;
				if ( strcasecmp( pszNotifyRelNameStart, pNotify->relname ) == 0 )
				{
					bIsStarted = true;
					write_err_msg("restore started for segment %d instance %d on host %s\n", 
								pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
				}
				else if ( strcasecmp( pszNotifyRelNameSucceed, pNotify->relname ) == 0 )
				{
					bIsFinished = true;
					pParm->bSuccess = true;
					write_err_msg("restore succeeded for segment %d instance %d on host %s\n", 
								pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
				}
				else if ( strcasecmp( pszNotifyRelNameFail, pNotify->relname ) == 0 )
				{
					g_b_SendCancelMessage = true;
					bIsFinished = true;
					pParm->bSuccess = false;
					// Make call to get error message from file on server
					pParm->pszErrorMsg = ReadBackendBackupFile( pConn, pInputOpts->pszBackupDirectory, pszKey, BFT_RESTORE_STATUS );
					
					write_err_msg("restore failed for segment %d instance %d on host %s\n", 
								pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost"));
				}
			}
			
			PQfreemem( pNotify );			
		}
		
		if ( nNotifies == 0 )
		{
			if ( !bIsStarted )
			{
				nTries++;
				if ( nTries == 10 )
				{
					bIsFinished = true;
					pParm->bSuccess = false;
					pParm->pszErrorMsg = MakeString( "restore failed to start for segment %d instance %d on host %s in the required time interval\n", 
												pSegInst->segid, pSegInst->instid, StringNotNull(pSegInst->pszHost, "localhost") );
					DoCancelNotifyListen( pConn, false, pszKey, pSegInst->instid, pSegInst->segid, NULL );
					bSentCancelMessage = true;
				}
			}
		}
	}
	
	free(pszNotifyRelName);
	free(pszNotifyRelNameStart);
	free(pszNotifyRelNameSucceed);
	free(pszNotifyRelNameFail);
	
	PQfinish(pConn);
	
	return(NULL);
}

bool restoreHead( InputOptions* pInputOpts, PGconn *pConn, SegmentInstance *pSegInst, ThreadParm* pParm )
{
	bool				rtn			= false;
	bool bForcePassword = false;
	bool bIgnoreVersion = false;
	
	memset( pParm, 0, sizeof(ThreadParm) );
	
	// We need to restore the head
	write_err_msg("Starting to restore the head.\n" );
		
	if (opts != NULL && opts->requirePassword)
		bForcePassword = true;

	if (opts != NULL && opts->ignoreVersion )
		bIgnoreVersion = true;
	
	// Now, spin off a thread to do the restore, and wait for it to finish
	pParm->thread				= 0;
	pParm->pSegInstData			= pSegInst; 
	pParm->pOptionsData			= pInputOpts;
	pParm->bSuccess				= false;
	pParm->pszErrorMsg			= NULL;
	pParm->pszRemoteBackupPath	= NULL;
	
	write_err_msg( "Creating thread to restore head: host %s port %d database %s\n",
			StringNotNull(pParm->pSegInstData->pszHost, "localhost"),
							pParm->pSegInstData->port,
							pParm->pSegInstData->pszDBName
			);
			
	pthread_create(&pParm->thread, 
					NULL, 
					threadProc, 
					pParm);
					
	pthread_join( pParm->thread, NULL);
	pParm->thread = (pthread_t)NULL;
	
	if ( pParm->bSuccess )
	{
		write_err_msg( "Successfully restored head: host %s port %d database %s\n",
						StringNotNull(pParm->pSegInstData->pszHost, "localhost"),
						pParm->pSegInstData->port,
						pParm->pSegInstData->pszDBName
					 );
		rtn = true;					 
	}
	else
	{
		if ( pParm->pszErrorMsg != NULL )
			write_err_msg( "with error: \n{%s}\n", pParm->pszErrorMsg );
		else
		{
			write_err_msg( "Failed to restore head: host %s port %d database %s\n",
							StringNotNull(pParm->pSegInstData->pszHost, "localhost"),
							pParm->pSegInstData->port,
							pParm->pSegInstData->pszDBName
							);
		}
	}
	
	return rtn;
}

/*
 * spinOffThreads: This function deals with all the threads that drive the backend restores.
 * First, it initializes the ThreadParmArray.  Then it loops and creates each of the threads.  
 * It then waits for all threads to finish. 
 */
void spinOffThreads(PGconn* pConn, InputOptions* pInputOpts, const SegmentInstanceArray* pSegInstAr, ThreadParmArray* pParmAr )
{
	int i;

	/* Create a thread and have it work on executing the dump on the
	* appropriate tail.
	*/
	if ( !createThreadParmArray( pSegInstAr->count, pParmAr ) )
	{
		write_err_msg( "Cannot allocate memory for thread parameters\n" );
		exit(-1);
	}
	
	for (i=0; i<pParmAr->count; i++)
	{
		ThreadParm* pParm = &pParmAr->pData[i];
		
		pParm->pSegInstData = &pSegInstAr->pData[i];
		pParm->pOptionsData = pInputOpts;
		pParm->bSuccess		= false;
        
        // exclude head node
        if (pParm->pSegInstData->instid == 1 || !pParm->pSegInstData->isValid)
            continue;
		
		write_err_msg( "Creating thread to restore segment %d instance %d: host %s port %d database %s\n",
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

	write_err_msg( "%s: Waiting for all remote %s programs to finish.\n",
				GetTimeNow(), pszAgent );
				
	/* Wait for all threads to complete */
	for (i=0; i<pParmAr->count; i++)
	{
		ThreadParm* pParm = &pParmAr->pData[i]; 
        // exclude head node and not valid node
        if (pParm->thread == (pthread_t)NULL)
            continue;
		pthread_join( pParm->thread, NULL);
		pParm->thread = (pthread_t)NULL;
	}
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
 * createThreadParmArray: This function initializes the count and pData elements of the ThreadParmArray. 
 */
bool createThreadParmArray( int nCount, ThreadParmArray* pParmAr)
{
	int i;

	pParmAr->count = nCount;
	pParmAr->pData = (ThreadParm *)malloc( nCount * sizeof(ThreadParm) );
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
 * reportRestoreResults: This function outputs a summary of the restore results to a file or to stdout
 */
int reportRestoreResults( const char* pszReportDirectory, const ThreadParm* pHeadParm, const ThreadParmArray* pParmAr )
{
	InputOptions* pOptions;
	FILE* fRptFile = NULL;
	FILE* fRpt = NULL;
	char* pszReportPathName = NULL;
	char* pszFormat;
	int i;
    int failCount;
	const ThreadParm* pParm;
	char* pszStatus;
	char* pszMsg;
	
	assert( pParmAr != NULL &&  pHeadParm != NULL );
	
	if ( pParmAr->count == 0 && pHeadParm->pOptionsData == NULL )
	{
		write_err_msg( "No results to report." );
		return 0;
	}
	
	pOptions = ( pParmAr->count > 0) ?  pParmAr->pData[0].pOptionsData : pHeadParm->pOptionsData;
	assert( pOptions != NULL );
	
	if ( pszReportDirectory == NULL || *pszReportDirectory == '\0' )
		fRpt = stdout;
	else
	{
		/* See whether we can create the file in the report directory */
		if ( pszReportDirectory[ strlen(pszReportDirectory) - 1] != '/'  )
			pszFormat = "%s/cdb_dump_%s.rpt";
		else
			pszFormat = "%scdb_dump_%s.rpt";
		
		pszReportPathName = MakeString( pszFormat, pszReportDirectory, pOptions->pszKey );
		
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
	fprintf( fRpt, "CDB Restore Report\n" );
	fprintf( fRpt, "Timestamp Key: %s\n", pOptions->pszKey );

	fprintf( fRpt, "cdb_restore Command Line: %s\n",  
			StringNotNull( pOptions->pszCmdLineParms, "None" ) );

	fprintf( fRpt, "Pass through Command Line Options: %s\n",  
			StringNotNull( pOptions->pszPassThroughParms, "None" ) );
	
	fprintf( fRpt, "Compression Program: %s\n", 
			StringNotNull( pOptions->pszCompressionProgram, "None" ) );
	
	fprintf( fRpt, "\n" );
	fprintf( fRpt, "Individual Results\n" );
	
    failCount=0;
	for ( i=0; i<pParmAr->count + 1; i++ )
	{
		if ( i == 0 )
		{
			if ( pHeadParm->pOptionsData != NULL )
				pParm = pHeadParm;
			else
				continue;
		}
		else
		{
			pParm = &pParmAr->pData[i-1];
            if (pParm->pSegInstData->instid == 1)
                continue;
		}
		
		if (!pParm->bSuccess)
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


