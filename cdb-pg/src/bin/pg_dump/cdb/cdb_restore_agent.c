/*-------------------------------------------------------------------------
 *
 * pg_restore.c
 *	pg_restore is an utility extracting postgres database definitions
 *	from a backup archive created by pg_dump using the archiver
 *	interface.
 *
 *	pg_restore will read the backup archive and
 *	dump out a script that reproduces
 *	the schema of the database in terms of
 *		  user-defined types
 *		  user-defined functions
 *		  tables
 *		  indexes
 *		  aggregates
 *		  operators
 *		  ACL - grant/revoke
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 * Basic process in a restore operation is:
 *
 *	Open the Archive and read the TOC.
 *	Set flags in TOC entries, and *maybe* reorder them.
 *	Generate script to stdout
 *	Exit
 *
 * Copyright (c) 2000, Philip Warner
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		$PostgreSQL: pgsql/src/bin/pg_dump/pg_restore.c,v 1.68 2004/12/03 18:48:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "../pg_backup.h"
#include "../pg_backup_archiver.h"
#include "../dumputils.h"

#include <ctype.h>

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include <unistd.h>

#include "getopt_long.h"
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include "cdb_dump_util.h"
#include "cdb_backup_status.h"


#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif

#ifdef ENABLE_NLS
#include <locale.h>
#endif

#define _(x) gettext((x))

/* Forward decls */
static void usage(const char *progname);

typedef struct option optType;

// cdb specific stuff
extern void makeSureMonitorThreadEnds( int TaskRC, char* pszErrorMsg );
static void myChildHandler(int signo);
static void myHandler(int signo);
static void* monitorThreadProc(void *arg);
static void _check_database_version(ArchiveHandle *AH, bool ignoreVersion);
static bool execCDBCatalogFunction(PGconn* pConn);

bool bUsePSQL = false;
bool bPSQLDone = false;
static char*            g_CDBDumpInfo = NULL;
static char*            g_CDBDumpKey = NULL;
static int              g_CDBSegID = 0;
static char*            g_CDBPassThroughCredentials = NULL;
static char*            g_pszCDBOutputDirectory = NULL;
static PGconn*          g_conn_status = NULL;
static pthread_t        g_main_tid = (pthread_t)0;
static pthread_t        g_monitor_tid = (pthread_t)0;
static bool             g_bOnErrorStop = false;
PGconn*                 g_conn = NULL;


static StatusOpList* g_pStatusOpList = NULL;
char*                   g_LastMsg = NULL;

int              g_CDBInstID = 0;

/* end cdb additions */
// end cdb specific stuff

int
main(int argc, char **argv)
{
	RestoreOptions *opts;
	int			c;
	int			exit_code=0;
	Archive    *AH;
	char	   *inputFileSpec;
	extern int	optind;
	extern char *optarg;
	static int	use_setsessauth = 0;
	static int	disable_triggers = 0;
    SegmentInstance SegInst;
    StatusOp* pOp;
    struct sigaction act, oact;
    pid_t newpid;
    char* pszOnErrorStop    = "-v ON_ERROR_STOP=";
    int len;  /* psql */
    int i;
    char* pszCmdLine;
    int status;
    int rc;
    char* pszErrorMsg;
    ArchiveHandle* pAH;

	struct option cmdopts[] = {
		{"clean", 0, NULL, 'c'},
		{"create", 0, NULL, 'C'},
		{"data-only", 0, NULL, 'a'},
		{"dbname", 1, NULL, 'd'},
		{"exit-on-error", 0, NULL, 'e'},
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
		{"password", 0, NULL, 'W'},
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
        {"cdb-d", required_argument, NULL, 1},
        {"cdb-e", no_argument, NULL, 2},
        {"cdb-k", required_argument, NULL, 3},

		{NULL, 0, NULL, 0}
	};

	set_pglocale_pgservice(argv[0], "pg_dump");

	opts = NewRestoreOptions();

    /* set format default */
    opts->formatName = "p";

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_restore (Bizgres MPP) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "acCd:ef:F:h:iI:lL:Op:P:RsS:t:T:uU:vWxX:",
							cmdopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				opts->dataOnly = 1;
				break;
			case 'c':			/* clean (i.e., drop) schema prior to
								 * create */
				opts->dropSchema = 1;
				break;
			case 'C':
				opts->create = 1;
				break;
			case 'd':
				opts->dbname = strdup(optarg);
				break;
			case 'e':
				opts->exit_on_error = true;
				break;
			case 'f':			/* output file name */
				opts->filename = strdup(optarg);
				break;
			case 'F':
				if (strlen(optarg) != 0)
					opts->formatName = strdup(optarg);
				break;
			case 'h':
				if (strlen(optarg) != 0)
					opts->pghost = strdup(optarg);
				break;
			case 'i':
				opts->ignoreVersion = 1;
				break;

			case 'l':			/* Dump the TOC summary */
				opts->tocSummary = 1;
				break;

			case 'L':			/* input TOC summary file name */
				opts->tocFile = strdup(optarg);
				break;

			case 'O':
				opts->noOwner = 1;
				break;
			case 'p':
				if (strlen(optarg) != 0)
					opts->pgport = strdup(optarg);
				break;
			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;
			case 'P':			/* Function */
				opts->selTypes = 1;
				opts->selFunction = 1;
				opts->functionNames = strdup(optarg);
				break;
			case 'I':			/* Index */
				opts->selTypes = 1;
				opts->selIndex = 1;
				opts->indexNames = strdup(optarg);
				break;
			case 'T':			/* Trigger */
				opts->selTypes = 1;
				opts->selTrigger = 1;
				opts->triggerNames = strdup(optarg);
				break;
			case 's':			/* dump schema only */
				opts->schemaOnly = 1;
				break;
			case 'S':			/* Superuser username */
				if (strlen(optarg) != 0)
					opts->superuser = strdup(optarg);
				break;
			case 't':			/* Dump data for this table only */
				opts->selTypes = 1;
				opts->selTable = 1;
				opts->tableNames = strdup(optarg);
				break;

			case 'u':
				opts->requirePassword = true;
				opts->username = simple_prompt("User name: ", 100, true);
				break;

			case 'U':
				opts->username = optarg;
				break;

			case 'v':			/* verbose */
				opts->verbose = 1;
				break;

			case 'W':
				opts->requirePassword = true;
				break;

			case 'x':			/* skip ACL dump */
				opts->aclsSkip = 1;
				break;

			case 'X':
				if (strcmp(optarg, "use-set-session-authorization") == 0)
					use_setsessauth = 1;
				else if (strcmp(optarg, "disable-triggers") == 0)
					disable_triggers = 1;
				else
				{
					fprintf(stderr,
							_("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				break;

				/* This covers the long options equivalent to -X xxx. */
			case 0:
				break;

            case 1:         /* --cdb-d CDB Output Directory */
                g_pszCDBOutputDirectory = strdup(optarg);
                break;

            case 2:         /* --cdb-e On Error Stop for psql */
                g_bOnErrorStop = opts->exit_on_error = true;
                break;

            case 3:         /* --cdb-k CDB Dump Info Format is Key_seg_inst*/
                g_CDBDumpInfo = strdup(optarg);
                if ( !ParseCDBDumpInfo( g_CDBDumpInfo, &g_CDBDumpKey, &g_CDBInstID, &g_CDBSegID, &g_CDBPassThroughCredentials ) )
                {
                    exit(1);
                }
                break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind < argc)
		inputFileSpec = argv[optind];
	else
		inputFileSpec = NULL;

	/* Should get at most one of -d and -f, else user is confused */
	if (opts->dbname)
	{
		if (opts->filename)
		{
			fprintf(stderr, _("%s: cannot specify both -d and -f output\n"),
					progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
		opts->useDB = 1;
	}

	opts->disable_triggers = disable_triggers;
	opts->use_setsessauth = use_setsessauth;

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
                bUsePSQL = true;
                break;

			default:
				write_msg("unrecognized archive format '%s'; please specify 't' or 'c'\n",
						  opts->formatName);
				exit(1);
		}
	}

    if ( g_CDBDumpInfo == NULL )
    {
        write_msg(NULL, "missing required parameter cdb-k (backup key)\n");
        exit(1);
    }
   
     /* cdb additions */
    SegInst.segid       = g_CDBSegID;
    SegInst.instid      = g_CDBInstID;
    SegInst.port        = opts->pgport ? atoi( opts->pgport ) : 5432;
    SegInst.isValid     = true;
    SegInst.pszHost     = opts->pghost ? strdup(opts->pghost) : NULL;
    SegInst.pszDBName   = opts->dbname ? strdup(opts->dbname) : NULL;
    SegInst.pszDBUser   = opts->username ? strdup(opts->username) : NULL;
    SegInst.pszDBPswd   = NULL;

    if ( g_CDBPassThroughCredentials != NULL && *g_CDBPassThroughCredentials != '\0' )
    {
        unsigned int nBytes;
        char* pszDBPswd = Base64ToData( g_CDBPassThroughCredentials, &nBytes );
        if ( pszDBPswd == NULL )
        {
            write_msg(NULL, "Invalid CDB Credentials:  %s\n", g_CDBPassThroughCredentials );
            exit(1);
        }
        if ( nBytes > 0 )
        {
            SegInst.pszDBPswd = malloc( nBytes + 1 );
            if ( SegInst.pszDBPswd == NULL )
            {
                write_msg(NULL, "Cannot allocate memory for CDB Credentials\n" );
                exit(1);
            }

            memcpy( SegInst.pszDBPswd, pszDBPswd, nBytes );
            SegInst.pszDBPswd[nBytes] = '\0';
        }
    }

    if (g_CDBInstID == 1)
        g_conn = MakeDBConnection( &SegInst, true );
    else
        g_conn = MakeDBConnection( &SegInst, false );
    if (PQstatus(g_conn) == CONNECTION_BAD)
    {
        exit_horribly(NULL, NULL, "connection to database \"%s\" failed: %s",
                   PQdb(g_conn), PQerrorMessage(g_conn));
    }

    if ( g_CDBDumpKey != NULL )
    {       
        /*  
         * Open the database again, for writing status info
         */     
        g_conn_status = MakeDBConnection( &SegInst, false );
                    
        if ( PQstatus(g_conn_status) == CONNECTION_BAD )
        {           
            exit_horribly(NULL, NULL, "Connection on host %s failed: %s",
                            StringNotNull(SegInst.pszHost, "localhost"),
                            PQerrorMessage(g_conn_status) );
        }   

/*
        res = PQexec(g_conn_status, "SET mpp_role=UTILITY");
        if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
        {       
            exit_horribly(NULL, NULL, "SET command failed: %s",
                          PQerrorMessage(g_conn_status));
        }

        PQclear(res);
*/

        g_main_tid = pthread_self();

        g_pStatusOpList = CreateStatusOpList();
        if ( g_pStatusOpList == NULL )
        {
            exit_horribly(NULL, NULL, "cannot allocate memory for cdb_backup_status operation\n");
        }

        // Start monitoring for cancel requests
        pthread_create(&g_monitor_tid,
                        NULL,
                        monitorThreadProc,
                        NULL );

        // Install Ctrl-C interrupt handler, now that we have a connection

        if ( !bUsePSQL )
        {
            struct sigaction act, oact;

            act.sa_handler = myHandler;
            sigemptyset(&act.sa_mask);
            act.sa_flags = 0;
            act.sa_flags |= SA_RESTART;
            if ( sigaction(SIGINT, &act, &oact) < 0 )
            {
                write_msg(NULL, "Error trying to set SIGINT interrupt handler\n");
            }

            act.sa_handler = myHandler;
            sigemptyset(&act.sa_mask);
            act.sa_flags = 0;
            act.sa_flags |= SA_RESTART;
            if ( sigaction(SIGTERM, &act, &oact) < 0 )
            {
                write_msg(NULL, "Error trying to set SIGTERM interrupt handler\n");
            }
        }

        pOp = CreateStatusOp( TASK_START, TASK_RC_SUCCESS, SUFFIX_START, TASK_MSG_SUCCESS );
        if ( pOp == NULL )
        {
            exit_horribly(NULL, NULL, "cannot allocate memory for cdb_backup_status operation\n");
        }
        AddToStatusOpList( g_pStatusOpList, pOp );
    }
    /* end cdb additions */

    if ( bUsePSQL )
    {
        // Install Ctrl-C interrupt handler, now that we have a connection
        act.sa_handler = myChildHandler;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;
        act.sa_flags |= SA_RESTART;
        if ( sigaction(SIGCHLD, &act, &oact) < 0 )
        {
            write_msg(NULL, "Error trying to set SIGCHLD interrupt handler\n");
            exit(1);
        }

        newpid = fork();
        if ( newpid < 0 )
        {
            write_msg(NULL, "failed to fork in cdb_backup\n");
        }
        else if ( newpid == 0 )
        {
            pszOnErrorStop    = "-v ON_ERROR_STOP=";
//          char* pszOutFile        = "~/cdb_restore.out";

            /* Child Process */
            // launch psql, wait for it to finish
            len = 4;  /* psql */
            for ( i=1; i<argc - 1; i++ )
            {
                if ( strncmp( argv[i], "--cdb-", 6 ) == 0 )
                {
                    if ( strcmp( argv[i], "--cdb-e" ) != 0 )
                        i++;
                    continue;
                }

                len += strlen( argv[i] ) + 1;
            }

            len += strlen( inputFileSpec ) + 4;
            if ( g_bOnErrorStop )
                len += strlen( pszOnErrorStop ) + 1;

    //      len += 2 + strlen( pszOutFile );  // " >"

            pszCmdLine = (char *) calloc( len+1, 1 );
            strcpy( pszCmdLine, "psql" );
            for ( i=1; i<argc-1; i++ )
            {
                if ( strncmp( argv[i], "--cdb-", 6 ) == 0 )
                {
                    if ( strcmp( argv[i], "--cdb-e" ) != 0 )
                        i++;
                    continue;
                }

                strcat( pszCmdLine, " ");
                strcat( pszCmdLine, argv[i] );
            }
            strcat( pszCmdLine, " -f " );
            strcat( pszCmdLine, inputFileSpec );

            if ( g_bOnErrorStop )
            {
                strcat( pszCmdLine, " ");
                strcat( pszCmdLine, pszOnErrorStop );
            }

//          strcat( pszCmdLine, " >" );
//          strcat( pszCmdLine, pszOutFile );

            if ( g_CDBInstID == 0 )
                putenv( "PGOPTIONS=-c mpp_session_role=UTILITY" );

            write_msg(NULL, "Command Line: %s\n", pszCmdLine );

            execl("/bin/sh", "sh", "-c", pszCmdLine, NULL );

            write_msg(NULL, "Error in cdb_restore_agent - execl of %s with Command Line %s failed",
                    "/bin/sh", pszCmdLine );
            _exit(127);
        }
        else
        {
            // Parent .  Lets sleep and wake up until we see it's done
            while ( !bPSQLDone )
            {
                sleep( 5 );
            }

            waitpid( newpid, &status, 0 );
            if ( WIFEXITED(status) )
            {
                rc = WEXITSTATUS(status);
                write_msg(NULL, "psql finished with rc %d.\n", rc );
                if ( rc != 0 )
                {
                    pszErrorMsg = MakeString( "psql finished abnormally with return code %d.\n", rc  );

                    makeSureMonitorThreadEnds( TASK_RC_FAILURE, pszErrorMsg );
                    free( pszErrorMsg );
                    exit_code=2;
                }
            }
            else if( WIFSIGNALED(status) )
            {
                pszErrorMsg = MakeString( "psql finished abnormally with signal number %d.\n", WTERMSIG(status)  );
                write_msg(NULL, pszErrorMsg);
                makeSureMonitorThreadEnds( TASK_RC_FAILURE, pszErrorMsg );
                free( pszErrorMsg );
                exit_code=2;
            }
            else
            {
                pszErrorMsg = MakeString( "psql crashed or finished badly.\n" );
                write_msg(NULL, pszErrorMsg);
                makeSureMonitorThreadEnds( TASK_RC_FAILURE, pszErrorMsg );
                free( pszErrorMsg );
                exit_code=2;
            }
        }
    }
    else
    {
		AH = OpenArchive(inputFileSpec, opts->format);

		/* Let the archiver know how noisy to be */
		AH->verbose = opts->verbose;

		/*
		 * Whether to keep submitting sql commands as "pg_restore ... | psql
		 * ... "
		 */
		AH->exit_on_error = opts->exit_on_error;

		if (opts->tocFile)
			SortTocFromFile(AH, opts);

		if (opts->tocSummary)
			PrintTOCSummary(AH, opts);
		else
        {
            pAH = (ArchiveHandle *) AH;

            if (opts->useDB)
            {
                /* check for version mismatch */
                if (pAH->version < K_VERS_1_3)
                    die_horribly(NULL, NULL, "direct database connections are not supported in pre-1.3 archives\n");

                pAH->connection = g_conn;
                /* XXX Should get this from the archive */
                AH->minRemoteVersion = 070100;
                AH->maxRemoteVersion = 999999;

                _check_database_version(pAH, opts->ignoreVersion);
            }

            RestoreArchive(AH, opts);
            /* The following is necessary when the -C option is used.  A new connection
             * is gotten to the database within RestoreArchive
             */
            if ( pAH->connection != g_conn )
                g_conn = pAH->connection;
        }

		/* done, print a summary of ignored errors */
		if (AH->n_errors)
			fprintf(stderr, _("WARNING: errors ignored on restore: %d\n"),
					AH->n_errors);

		/* AH may be freed in CloseArchive? */
		exit_code = AH->n_errors ? 1 : 0;

		CloseArchive(AH);
    }

    /*
    if ( g_CDBDumpKey != NULL &&
        !opts->dataOnly &&
        g_CDBInstID == 0 && g_CDBSegID == 0)
    {
        execCDBCatalogFunction(g_conn);
    }
    */

    makeSureMonitorThreadEnds( TASK_RC_SUCCESS, TASK_MSG_SUCCESS );

    DestroyStatusOpList(g_pStatusOpList);

    if ( SegInst.pszHost )
        free( SegInst.pszHost );
    if ( SegInst.pszDBName )
        free( SegInst.pszDBName );
    if ( SegInst.pszDBUser )
        free( SegInst.pszDBUser );
    if ( SegInst.pszDBPswd )
        free( SegInst.pszDBPswd );

    PQfinish(g_conn);
    if (exit_code==0)
        write_msg(NULL, "Finished successfully\n");
    else
        write_msg(NULL, "Finished with errors\n");

	exit(0);
}

static void
usage(const char *progname)
{
	printf(_("%s restores a PostgreSQL database from an archive created by pg_dump.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [FILE]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -d, --dbname=NAME        connect to database name\n"));
	printf(_("  -f, --file=FILENAME      output file name\n"));
	printf(_("  -F, --format=c|t         specify backup file format\n"));
	printf(_("  -i, --ignore-version     proceed even when server version mismatches\n"));
	printf(_("  -l, --list               print summarized TOC of the archive\n"));
	printf(_("  -v, --verbose            verbose mode\n"));
	printf(_("  --help                   show this help, then exit\n"));
	printf(_("  --version                output version information, then exit\n"));

	printf(_("\nOptions controlling the restore:\n"));
	printf(_("  -a, --data-only          restore only the data, no schema\n"));
	printf(_("  -c, --clean              clean (drop) schema prior to create\n"));
	printf(_("  -C, --create             create the target database\n"));
	printf(_("  -I, --index=NAME         restore named index\n"));
	printf(_("  -L, --use-list=FILENAME  use specified table of contents for ordering\n"
			 "                           output from this file\n"));
	printf(_("  -O, --no-owner           skip restoration of object ownership\n"));
	printf(_("  -P, --function=NAME(args)\n"
			 "                           restore named function\n"));
	printf(_("  -s, --schema-only        restore only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME     specify the superuser user name to use for\n"
			 "                           disabling triggers\n"));
	printf(_("  -t, --table=NAME         restore named table\n"));
	printf(_("  -T, --trigger=NAME       restore named trigger\n"));
	printf(_("  -x, --no-privileges      skip restoration of access privileges (grant/revoke)\n"));
	printf(_("  -X disable-triggers, --disable-triggers\n"
			 "                           disable triggers during data-only restore\n"));
	printf(_("  -X use-set-session-authorization, --use-set-session-authorization\n"
			 "                           use SESSION AUTHORIZATION commands instead of\n"
			 "                           OWNER TO commands\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  -e, --exit-on-error      exit on error, default is to continue\n"));

    printf(("\nCDB specific options:\n"));
    printf(("   --cdb-k=BACKUPKEY        key for CDB Backup\n"));

	printf(_("\nIf no input file name is supplied, then standard input is used.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}


void myChildHandler(int signo)
{
	bPSQLDone = true;
}

/*
 * makeSureMonitorThreadEnds: This function adds a request for a TASK_FINISH
 * status row, and then waits for the g_monitor_tid to end.
 * This will happen as soon as the monitor thread processes this request. 
 */  
void makeSureMonitorThreadEnds( int TaskRC, char* pszErrorMsg )
{
	StatusOp* pOp;
	if ( g_monitor_tid )
	{
		char* pszSuffix;
		if ( TaskRC == TASK_RC_SUCCESS )
			pszSuffix = SUFFIX_SUCCEED;
		else
			pszSuffix = SUFFIX_FAIL;
			
		pOp = CreateStatusOp( TASK_FINISH, TaskRC, pszSuffix, pszErrorMsg );
		if ( pOp == NULL )
		{
			/* This is really bad, since the only way we cab shut down gracefully is
			 * if we can insert a row into the ststus table.  Oh well,
			 * at least log a message and exit.
			 */
			 write_msg(NULL, "*** aborted because of fatal memory allocation error\n");
			 exit(1);
		}
		else
			AddToStatusOpList( g_pStatusOpList, pOp );
	
		pthread_join( g_monitor_tid, NULL );
	}
}
/*
 * monitorThreadProc: This function runs in a thread and owns the other connection to the database. g_conn_status
 * It listens using this connection for notifications.  A notification only arrives if we are to 
 * cancel. In that case, we cause the interrupt handler to be called by sending ourselves a SIGINT
 * signal.  The other duty of this thread, since it owns the g_conn_status, is to periodically examine
 * a linked list of cdb_backup_status insert command requests.  If any are there, it causes an insert and a notify
 * to be sent.    
 */
void* monitorThreadProc(void *arg)
{
	int sock;
	bool bGotFinished;
	bool bGotCancelRequest;
	int PID;
	struct timeval tv;
	int nNotifies;
	bool bFromOtherProcess;
	StatusOp* pOp;
	char* pszMsg;
	fd_set input_mask;
	PGnotify* notify;
	
	// Block SIGINT and SIGKILL signals (we want them only ion main thread)
	sigset_t newset;
	sigemptyset( &newset );
	sigaddset( &newset, SIGINT );
	sigaddset( &newset, SIGKILL );
	pthread_sigmask( SIG_SETMASK, &newset, NULL );
	
	write_msg(NULL, "Starting monitor thread at %s\n", GetTimeNow() );
	
	/* Issue Listen command  */
	DoCancelNotifyListen( g_conn_status, true, g_CDBDumpKey, g_CDBInstID, g_CDBSegID, NULL );
	
	// Now wait for a cancel notification
	sock = PQsocket( g_conn_status );
	FD_ZERO( &input_mask );
	FD_SET( sock, &input_mask );
	bGotFinished = false;
	bGotCancelRequest = false;
	PID = PQbackendPID( g_conn_status );

	/* Once we've seen the TASK_FINISH insert request, we know to leave */ 	
	while ( !bGotFinished )
	{
		tv.tv_sec = 2;
		tv.tv_usec = 0;
	
		if ( select( sock+1, &input_mask, NULL, NULL, &tv ) < 0 )
		{
			write_msg(NULL, "select failed for backup key %s, instid %d, segid %d failed\n",
					  g_CDBDumpKey, g_CDBInstID, g_CDBSegID);
			bGotCancelRequest = true;
		}
	
		if ( PQstatus( g_conn_status ) == CONNECTION_BAD )
		{
			write_msg(NULL, "Status Connection went down for backup key %s, instid %d, segid %d\n",
					  g_CDBDumpKey, g_CDBInstID, g_CDBSegID);
			bGotCancelRequest = true;
		}
		
		PQconsumeInput( g_conn_status );
		nNotifies = 0;
		bFromOtherProcess = false;
		while ( NULL != (notify = PQnotifies(g_conn_status)))
		{
			nNotifies++;

			/* Need to make sure that the notify is from another process, since this processes
			 * also issues notifies that aren't cancel requests. 
			 */
			if ( notify->be_pid !=  PID )
				bFromOtherProcess = true;
			
			PQfreemem( notify );			
		}
			
		if ( nNotifies > 0 && bFromOtherProcess )
			bGotCancelRequest = true;
		
		if ( bGotCancelRequest )
		{
			write_msg(NULL, "Notification received that we need to cancel for backup key %s, instid %d, segid %d failed\n",
					  g_CDBDumpKey, g_CDBInstID, g_CDBSegID);
			
			pthread_kill( g_main_tid, SIGINT );
		}
		
		/* Now deal with any insert requests */
		Lock( g_pStatusOpList );
		while ( NULL != (pOp = TakeFromStatusOpList(g_pStatusOpList)) )
		{
			DoCancelNotifyListen( g_conn_status, false, g_CDBDumpKey, g_CDBInstID, g_CDBSegID, pOp->pszSuffix );
			
			if ( pOp->TaskID == TASK_FINISH )
			{
				bGotFinished = true;

				if ( pOp->TaskRC == TASK_RC_SUCCESS )
					pszMsg = "Succeeded\n";	
				else
					pszMsg = pOp->pszMsg;
				
				/* WriteStatusToFile( pszMsg, StringNotNull( g_pszCDBOutputDirectory, "." ), 
									g_CDBDumpKey, g_CDBInstID, g_CDBSegID, false ); */
                 write_msg(NULL, pszMsg);
			}
			
			if ( pOp->TaskID == TASK_SET_SERIALIZABLE )
			{
				write_msg(NULL, "TASK_SET_SERIALIZABLE at %s\n", GetTimeNow() );
			}
			
			FreeStatusOp( pOp );
		}
		
		Unlock( g_pStatusOpList );
	}
	
	/* Close the g_conn_status connection. */
	PQfinish(g_conn_status);
	
	return NULL;
}
/*
 * my_handler: This function is the signal handler for both SIGINT and SIGTERM signals.  
 * It checks to see whether there is an active transaction running on the g_conn connection
 * If so, it issues a PQrequestCancel.  This will cause the current statement to fail, which will
 * cause cdb_dump to terminate gracefully.
 * If there's no active transaction, it does nothing.
 * This is set up to always run in the context of the main thread.
 */
void myHandler(int signo)
{
	if ( g_conn != NULL &&  PQtransactionStatus( g_conn ) == PQTRANS_ACTIVE )
	{
		write_msg(NULL, "in my_handler before PQrequestCancel\n" );
		PQrequestCancel( g_conn );
	}
	else
	{
		write_msg(NULL, "in my_handler not in active transaction\n" );
	}
}

/* 
 * _check_database_version: This was copied here from pg_backup_db.c because it's declared static there.
 * We need to call it directly because in pg_dump.c it's called from ConnectDatabase
 * which we are not using. 
 */
void _check_database_version(ArchiveHandle *AH, bool ignoreVersion)
{
	int			myversion;
	const char *remoteversion_str;
	int			remoteversion;

	myversion = parse_version(PG_VERSION);
	if (myversion < 0)
		die_horribly(AH, NULL, "could not parse version string \"%s\"\n", PG_VERSION);

	remoteversion_str = PQparameterStatus(AH->connection, "server_version");
	if (!remoteversion_str)
		die_horribly(AH, NULL, "could not get server_version from libpq\n");

	remoteversion = parse_version(remoteversion_str);
	if (remoteversion < 0)
		die_horribly(AH, NULL, "could not parse version string \"%s\"\n", remoteversion_str);

	AH->public.remoteVersion = remoteversion;

	if (myversion != remoteversion
		&& (remoteversion < AH->public.minRemoteVersion ||
			remoteversion > AH->public.maxRemoteVersion))
	{
		write_msg(NULL, "server version: %s; %s version: %s\n",
				  remoteversion_str, progname, PG_VERSION);
		if (ignoreVersion)
			write_msg(NULL, "proceeding despite version mismatch\n");
		else
			die_horribly(AH, NULL, "aborting because of version mismatch  (Use the -i option to proceed anyway.)\n");
	}
}

/*
 * execCDBCatalogFunction:
 */
bool execCDBCatalogFunction(PGconn* pConn)
{
	const char* pszFunctionName		= "restore_cdb_catalog"; 
	const char* pszNamespaceName	= "cdb"; 
	bool		bRtn				= false;
	PQExpBuffer bufQry				= NULL;
	PGresult*	pRes				= NULL;
	int ntups;
	
	bufQry = createPQExpBuffer();
	
	appendPQExpBuffer( bufQry, "SELECT count(*) from pg_catalog.pg_proc p"
								" inner join pg_catalog.pg_namespace n"
								" on p.pronamespace=n.oid"
								" where p.proname='%s'"
								" and n.nspname='%s'",
						pszFunctionName, pszNamespaceName );

	pRes = PQexec(pConn, bufQry->data);
	if (!pRes || PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		write_err_msg("Query %s Command failed: %s",
					bufQry->data, PQerrorMessage(pConn));
		goto cleanup;
	}			
									
	ntups = PQntuples(pRes);
	if ( ntups == 0 )
	{
		write_err_msg("Query %s Command failed: %s",
					bufQry->data, PQerrorMessage(pConn));
		goto cleanup;
	}
	
	if ( 0 == atoi( PQgetvalue(pRes, 0, 0)))
	{
		write_err_msg("Cannot execute function %s.%s() because it does not exist",
					pszNamespaceName, pszFunctionName );
		goto cleanup;
	}
	
	PQclear(pRes);
	resetPQExpBuffer(bufQry);
	
	appendPQExpBuffer( bufQry, "SELECT %s.%s()",
						pszNamespaceName, pszFunctionName );

	pRes = PQexec(pConn, bufQry->data);
	if (!pRes || PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		write_err_msg("Cannot execute function %s.%s() because it does not exist",
					bufQry->data, PQerrorMessage(pConn) );
		goto cleanup;
	}
	
	bRtn = true;

cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	if ( bufQry != NULL )
		destroyPQExpBuffer(bufQry);
	
	return bRtn;
}

