#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

#include "postgres_fe.h"
#include "libpq-fe.h"
#include "libpq/pqsignal.h"
#include "gpupgrade_cmds.h"

typedef struct 
{
	int content;
	bool definedprimary;
	int dbid;
	bool isprimary;
	bool valid;
	char *hostname ;
	char *port ;
	char *dbname;
	PGconn *conn;
	PGresult *res;
	char *conninfo;
} config_t;

/* command line options */
static bool upgrade = true;
static bool verbose = false;
static char *myname = NULL; /* argv[0] */
static bool do_it = true; /* really do it? */

/* signal handling */
bool caught_signal = false;
bool query_failed = false;

#define GPUP_VERSION "3.1.0-1"

/* conninfo options */
#define CONNOPTIONS "options='-c gp_session_role=utility'"

/* kinds of logging messages */
#define UDEBUG 1
#define UINFO 2
#define UERROR 3

#define lev2str(lev) \
 (lev == UDEBUG ? "DEBUG" : (lev == UINFO ? "INFO" : "ERROR"))

static void mylog(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

	static void
mylog(int level, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	
	va_start(ap, fmt);

	vsnprintf(buf, sizeof(buf), fmt, ap);

	if (level == UDEBUG)
	{
		if (verbose)
		{
			printf("%s: %s\n", lev2str(level), buf);
			fflush(stdout);
		}
	}
	else if (level == UINFO)
	{
		printf("%s: %s\n", lev2str(level), buf);
		fflush(stdout);
	}
	else
	{
		fprintf(stderr, "%s: %s\n", lev2str(level), buf);
		fflush(stderr);
	}
}

/* 
 * Perform the ALTER TABLE operations necessary to upgrade from 3.0 to 3.1
 */
static void
schema_upgrade(config_t *conf)
{
	if (upgrade)
	{
		/* exttable */

		/* pg_window */

	}

}

static void
run_upgrade_commands(config_t *conf)
{

	return;
}

static void
MyNoticeProcessor (void *arg, const char *message)
{
	mylog(UINFO, message);
} 

static void
trapsig(int signum)
{
	/* handle systems that reset the handler, like Windows (grr) */
	pqsignal(signum, trapsig);
	caught_signal = true;
	mylog(UDEBUG, "received signal %i", signum);
}

/*
 * call exit_nicely() if we got a signal, or else output "ok".
 */
static void
check_ok(config_t **segdbs)
{
	if (caught_signal)
	{
		int i;

		mylog(UERROR, "received signal, rolling back upgrade");

		for (i = 0; segdbs[i] != NULL; i++)
		{
			if (segdbs[i]->conn)
				PQfinish(segdbs[i]->conn);
		}
		exit(1);
	}
	else if (query_failed)
	{
		if (segdbs)
		{
			int i;
		
			/* roll back all */
			mylog(UINFO, "rolling back cluster wide upgrade");
		
			for (i = 0; segdbs[i] ; i++)
			{
				PQfinish(segdbs[i]->conn);
			}
		}
		exit(1);
	}
	else
	{
		/* all seems well */
		mylog(UDEBUG, "ok");
	}
}

static PGresult *
run_query(PGconn *conn, char *str, char *host)
{
	PGresult *res;
	
	res = PQexec(conn, str);

	if (verbose)
		mylog(UDEBUG, str);

	if (!(PQresultStatus(res) == PGRES_COMMAND_OK ||
		  PQresultStatus(res) == PGRES_TUPLES_OK))
	{
		mylog(UERROR, "QUERY FAILED: \"%s\", error: \"%s\", "
			  "connection info: %s\n", str, PQerrorMessage(conn),
			  host ? host : "default host");
		PQclear(res);

		query_failed = true;

		return NULL;
	}
	return res;
}

static char *
ugstr(void)
{
	struct timeval tv;
	char *buf;

#define UG_STR_LEN 128
	
	buf = malloc(UG_STR_LEN);
	gettimeofday(&tv, NULL);
	strftime(buf, UG_STR_LEN, "gpuprade-%Y-%m-%d.%H:%M:%S", 
			 localtime(&tv.tv_sec));		 
	return buf;
}

static PGconn *
connect_to_db(char *conninfo, bool require_password)
{
	PGconn	 *conn;
	PGresult   *res;
	bool		need_pass = false;
	static char *password = NULL;
	
	if (require_password && !password)
		password = simple_prompt("Password: ", 100, false);
	
	do
	{
		need_pass = false;
		
		/* Make a connection to the database */
		
		if (password)
		{
		 	strcat(conninfo," password=");
		 	strcat(conninfo,password);
		}
			
		conn = PQconnectdb(conninfo);

		if (PQstatus(conn) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(conn), PQnoPasswordSupplied) == 0 &&
			!feof(stdin))
		{
			PQfinish(conn);
			need_pass = true;
			if (password)
				free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);
	
	 /* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		mylog(UERROR, "connection to database failed: %s", PQerrorMessage(conn));
		mylog(UERROR, "conninfo \"%s\"",conninfo);
		PQfinish(conn);
		exit(1);
	}
	
	PQsetNoticeProcessor(conn,
					 MyNoticeProcessor,
					 NULL);
	
	res = run_query(conn, "BEGIN;", conninfo);
	res = run_query(conn, "SET SEARCH_PATH = 'pg_catalog'", conninfo);

	PQclear(res);
	return conn;
}

#if 0
static void
run_sql_command(PGconn *conn, char *cmd)
{
	PGresult   *res;
	char		SqlState[6];
	
	if (verbose)
		printf("%s\n",cmd);
	res = PQexec(conn, cmd);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		 SqlState[0] = '\0';
		 if (PQresultErrorField(res, PG_DIAG_SQLSTATE) != NULL)
			strcpy(SqlState, PQresultErrorField(res, PG_DIAG_SQLSTATE));
			
		if (strcmp(SqlState,"42P07")!=0 &&  /* Table already exists */
			strcmp(SqlState,"42701")!=0)	/* Column already exists */
		{
			fprintf(stderr, " %s: command failed: \n%s %s\n", cmd,
					SqlState, PQerrorMessage(conn));

			PQclear(res);
			PQfinish(conn);
			fprintf(stderr," UPGRADE FAILED\n");
			exit(1);
		}
		PQclear(res);
	}
	else
		PQclear(res);
	
}

#endif

static void
help(void)
{
	/* The line below marks the start and end of the screen on an 80 col term
	 *      ===============================================================================
	 */
	printf("%s is used to upgrade a Greenplum DB 3.0 series system to a \n"
		   "Greenplum DB 3.1 series system.\n\n", myname);
	printf("%s must be run from the master database node after upgrading the \n", myname);
	printf("Greenplum Database software.\n\n");
	printf("Usage:\n");
	printf("  %s [OPTION]...\n", myname);

	printf("\nGeneral options:\n");
	printf(_("  --help                 show this help, then exit\n"));
	printf(_("  --version              output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -U, --username=NAME    connect as specified database user (must be superuser!)\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("  -d, --debug            verbose/debugging output\n"));
	printf(_("  -D, --downgrade        downgrade from 3.1 to 3.0\n"));
	printf(_("  -t, --test             do everything but the actual upgrade\n"));
	printf(_("                         (prints SQL, connection info, etc)\n"));
}

int
main(int argc, char **argv)
{
	char conninfo[1024];
	PGconn	 *conn;
	PGresult   *res;
	int		 i;
	config_t **segdbs;  
	int segdbcount = 0;
	
	
	static struct option long_options[] =
	{
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"version", no_argument, NULL, 'V'},
		{"password", no_argument, NULL, 'W'},
		{"debug", no_argument, NULL, 'd'},
		{"help", no_argument, NULL, '?'},
		{"downgrade", no_argument, NULL, 'D'},
		{"test", no_argument, NULL, 't'},
		{NULL, 0, NULL, 0}
	};

	int		 optindex;
	extern char *optarg;
	extern int  optind;
	int		 c;
	
	char *dbname = NULL;
	char *host = NULL;
	char *port = NULL;
	char *username = NULL;
	char *version = NULL;
	bool isMPP;
	bool force_password = false;

	char *txn_str = ugstr(); /* for 2PC */
	
	myname = argv[0];
	
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0)
		{
			help();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			fprintf(stderr, "%s (Greenplum Database) %s",
				   	myname, GPUP_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "h:p:U:d?tVW",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{

			case 'h':
				host = optarg;
				break;
			case 'p':
				port = optarg;
				break;
			case 'U':
				username = optarg;
				break;
			case 'W':
				force_password = true;
				break;
			case 'd':
				verbose = true;
				break;
			case '?':
				{
					help();
					exit(EXIT_SUCCESS);
				}
				break;
			case 't':
				do_it = false;
				break;
			case 'D':
				upgrade = false;
				break;
			default:
				mylog(UERROR, "unrecognised argument '%c'", c);
				exit(EXIT_FAILURE);
				break;
		}
	}
  
	/*
	 * if we still have arguments, use it as the username
	 */
	while (argc - optind >= 1)
	{
	   if (!username)
			username = argv[optind];
		else 
			mylog(UERROR, "extra command-line argument \"%s\" ignored",
				  argv[optind]);

		optind++;
	}
 
	/* some of these are not valid on Windows */
#ifdef SIGHUP
	pqsignal(SIGHUP, trapsig);
#endif
#ifdef SIGINT
	pqsignal(SIGINT, trapsig);
#endif
#ifdef SIGQUIT
	pqsignal(SIGQUIT, trapsig);
#endif
#ifdef SIGTERM
	pqsignal(SIGTERM, trapsig);
#endif

	/* Ignore SIGPIPE when writing to backend, so we can clean up */
#ifdef SIGPIPE
	pqsignal(SIGPIPE, SIG_IGN);
#endif

	/* initial DB we connect to */
	dbname = "template1";  


	/*
	 * If the user supplies a parameter on the command line, use it as the
	 * conninfo string; otherwise default to setting dbname=postgres and using
	 * environment variables or defaults for all other connection parameters.
	 */
	conninfo[0] = '\0';
	if (host)
	{
		strcat(conninfo, "host=");
	 	strcat(conninfo, host);
	}
	if (port)
	{
		strcat(conninfo," port=");
	 	strcat(conninfo, port);
	}
	if (dbname)
	{
	 	strcat(conninfo," dbname=");
	 	strcat(conninfo, dbname);
	}
	if (username)
	{
	 	strcat(conninfo," user=");
	 	strcat(conninfo, username);
	}
	 
	strcat(conninfo , " " CONNOPTIONS);
	
	mylog(UDEBUG, "%s running.", myname);

	/* Make a connection to the database */
	conn = connect_to_db(conninfo, force_password);
	
	res = run_query(conn, "SELECT VERSION()", host);

	/* we don't have any segs up yet */
	check_ok(NULL);

	version = PQgetvalue(res, 0, 0);
	
	if (strchr(version,')') != NULL)
	{
		char *ptr = strchr(version,')') + 1;
		*ptr = '\0';
	}

	mylog(UINFO, "version of system: %s", version);

	if (strstr(version,"Greenplum Database") != 0) 
		isMPP = 1;
	else
		isMPP = 0;
		
	if (!isMPP)
	{	
		mylog(UDEBUG, "This is not a Greenplum Database system!");
		PQfinish(conn);
		exit(1);
	}

	if (strncmp(version, "PostgreSQL 8.2.4",strlen("PostgreSQL 8.2.4")) != 0)
	{
		mylog(UERROR, "You must upgrade the Greenplum Database software to Greenplum Database 3.1");
		mylog(UERROR, "and restart the system, before running this utility\n");
		PQfinish(conn);
		exit(1);
		
	}
	
	PQclear(res);
	
	res = run_query(conn, "select content, definedprimary, dbid, isprimary,"
					"valid, hostname, port, datname from "
					"pg_catalog.gp_configuration "
					"cross join pg_catalog.pg_database where "
					"dbid = 1 or content >= 0 order by content," 
					"definedprimary desc, dbid;", host);

	segdbcount = PQntuples(res);

	/* 
	 * segdbs stores connections to each db on each segment. The last entry
	 * is NULL, so that we can loop until we find a NULL config.
	 */
	segdbs = malloc(sizeof(config_t *) * (segdbcount + 1));

	/* next, save the rows */
	for (i = 0; i < segdbcount; i++)
	{
		segdbs[i] = malloc(sizeof(config_t));
		segdbs[i]->content = atol(PQgetvalue(res, i, 0));
		segdbs[i]->definedprimary = PQgetvalue(res, i, 1)[0] == 't';
		segdbs[i]->dbid = atol(PQgetvalue(res, i, 2));
		segdbs[i]->isprimary = PQgetvalue(res, i, 3)[0] == 't';
		segdbs[i]->valid = PQgetvalue(res, i, 4)[0] == 't';
		
		if (i == 0 && segdbs[i]->content == -1 && host != NULL)
			segdbs[i]->hostname = host;
		else 
			segdbs[i]->hostname = strdup(PQgetvalue(res, i, 5));

		segdbs[i]->port = strdup(PQgetvalue(res, i, 6));
		segdbs[i]->dbname = strdup(PQgetvalue(res, i, 7));
		segdbs[i]->conn = NULL;
	}
	
	segdbs[i] = NULL;
	
	PQclear(res);
	
	segdbs[0]->conn = conn;
	
	check_ok(segdbs);

	mylog(UDEBUG, "got configuration information from entry db.");

	mylog(UDEBUG, "opening template0 databases.");
	
	/* allow access to template0 */
	for (i = 0; i < segdbcount; i++)
	{
		config_t *conf = segdbs[i];
		
		if (strcmp(conf->dbname, "template1") != 0)
			continue;
		
		mylog(UDEBUG, "updating segdb content %d definedprimary %d dbid %d",
			  conf->content,conf->definedprimary,conf->dbid);

		sprintf(conninfo,"host=%s port=%s dbname=%s " CONNOPTIONS,
				conf->hostname, conf->port, conf->dbname);
	
		/* Make a connection to the database */
	   	conf->conn = connect_to_db(conninfo, false);

		check_ok(segdbs);
		
   		run_query(conf->conn, "update pg_database set "
				  "datallowconn = 't' where datname='template0'",
				  conf->hostname);

		check_ok(segdbs);

		/* 
		 * We must commit now and re-open a transaction block, so that
		 * our update is visible.
		 */

		run_query(conf->conn, "commit;", conf->hostname);
		check_ok(segdbs);
		run_query(conf->conn, "begin;", conf->hostname);
		check_ok(segdbs);
   	}

	for (i = 0; i < segdbcount; i++)
	{
		config_t *conf = segdbs[i];
		char conninfo[1024];
		
		mylog(UINFO, "upgrading segment %s:%s (%s)", conf->hostname,
			   conf->port, conf->dbname);

		if (!conf->conn)
		{
			sprintf(conninfo,"host=%s port=%s dbname=%s " CONNOPTIONS,
					conf->hostname, conf->port, conf->dbname);
	
			/* Make a connection to the database */
		   	conf->conn = connect_to_db(conninfo, false);
		}
	}
	
	/*
	 * We now have a connection open to the database on every segDB 
	 * (0 is masterDB)
	 */
	for (i = 0; i < segdbcount; i++)
	{
		conn = segdbs[i]->conn;
		check_ok(segdbs);

		mylog(UDEBUG, "updating segdb content %d definedprimary %d dbid "
				   "%d for database %s",segdbs[i]->content,
				   segdbs[i]->definedprimary, segdbs[i]->dbid,
				   segdbs[i]->dbname);

		/* modify schema */
		schema_upgrade(segdbs[i]);

		/* run the upgrade command */
		run_upgrade_commands(segdbs[i]);
			
	}
	
	for (i = 0; i < segdbcount; i++)
	{ 	

		if (strcmp(segdbs[i]->dbname, "template1") != 0)
			continue;
		
		mylog(UDEBUG, "updating segdb content %d definedprimary %d dbid %d",
				   segdbs[i]->content, segdbs[i]->definedprimary,
				   segdbs[i]->dbid);
	
		conn = segdbs[i]->conn;
		run_query(conn, "update pg_database set datallowconn = 'f' "
						"where datname='template0'", segdbs[i]->hostname);
		check_ok(segdbs);
	}

	/* prepare commit and commit */
	for (i = 0; i < segdbcount; i++)
	{	
		char buf[256];
		snprintf(buf, sizeof(buf), "prepare transaction '%s-%s';",
				 txn_str, segdbs[i]->dbname);
		
		run_query(segdbs[i]->conn, buf, segdbs[i]->hostname);
		check_ok(segdbs);
	}

	for (i = 0; i < segdbcount; i++)
	{	
		char buf[256];
		snprintf(buf, sizeof(buf), "commit prepared '%s-%s';",
				 txn_str, segdbs[i]->dbname);
		
		run_query(segdbs[i]->conn, buf, segdbs[i]->hostname);
	}
	
	for (i = 0; i < segdbcount; i++)
	{	
		if (strcmp(segdbs[i]->dbname, "template1") == 0 ||
			strcmp(segdbs[i]->dbname,"template0") == 0)
		{
			run_query(conn, "VACUUM FULL", segdbs[i]->hostname);
			run_query(conn, "VACUUM FREEZE", segdbs[i]->hostname);
		}
	}
	printf("Done\n");
	/* close the connection to the database and cleanup */
	PQfinish(conn);
	return(0);	
}
