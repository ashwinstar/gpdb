#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "postgres_fe.h"
#include "libpq-fe.h"

/* command line options */
static bool upgrade = true;
static bool verbose = false;
static char *myname = NULL; /* argv[0] */
static bool do_it = true; /* really do it? */

#define GPUP_VERSION "3.1.0-1"

static void MyNoticeProcessor (void *arg, const char *message)
{
	fprintf(stderr, "%s", message);
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
		fprintf(stderr, "Connection to database failed: %s\n"
				"Using conninfo \"%s\"\n",
				PQerrorMessage(conn),conninfo);
		PQfinish(conn);
		exit(1);
	}
	
	PQsetNoticeProcessor(conn,
					 MyNoticeProcessor,
					 NULL);
	
	res = PQexec(conn, "SET SEARCH_PATH = 'pg_catalog'");

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "SET SEARCH_PATH failed: %s", PQerrorMessage(conn));
		PQclear(res);
	   	PQfinish(conn);
		exit(1);
	}
	PQclear(res);
	return conn;
}

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

static void
help(void)
{
	/* The line below marks the start and end of the screen on an 80 col term
	 *      ===============================================================================
	 */
	printf("%s is used to upgrade a Greenplum DB 3.0 series system to a Greenplum DB\n", myname);
	printf("3.1 series system.\n\n");
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
	printf(_("                         (prints SQL, connection info, etc)"));
}

int
main(int argc, char **argv)
{
	char conninfo[1024];
	PGconn	 *conn;
	PGresult   *res;
	int		 i;

	
	typedef struct 
	{
		int content;
		bool definedprimary;
		int dbid;
		bool isprimary;
		bool valid;
		char *hostname ;
		char *port ;
		PGconn *conn;
		PGresult *res;
	} config_t;
		   
		   
	config_t *segdbs;  
	int segdbcount = 0;
	
	char **databasenames;
	int	databasecount = 0;
	
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
	int d;
	
	char * dbname = 0;
	char * host = 0;
	char * port = 0;
	char * username = 0;

	char * version = 0;
	bool isMPP;
	bool force_password = false;
	
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

	while ((c = getopt_long(argc, argv, "h:p:U:vVW",
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
			case 'v':
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
				fprintf(stderr, "unrecognised argument '%c'\n", c);
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
			fprintf(stderr, "%s: warning: extra command-line argument \"%s\" ignored\n",
					argv[0], argv[optind]);

		optind++;
	}
 
	dbname = "template1";  


	/*
	 * If the user supplies a parameter on the command line, use it as the
	 * conninfo string; otherwise default to setting dbname=postgres and using
	 * environment variables or defaults for all other connection parameters.
	 */
	 
	 conninfo[0] = '\0';
	 if (host)
	 {
	 	strcat(conninfo,"host=");
	 	strcat(conninfo,host);
	 }
	 if (port)
	 {
	 	strcat(conninfo," port=");
	 	strcat(conninfo,port);
	 }
	 if (dbname)
	 {
	 	strcat(conninfo," dbname=");
	 	strcat(conninfo,dbname);
	 }
	 if (username)
	 {
	 	strcat(conninfo," user=");
	 	strcat(conninfo,username);
	 }
	 
	 

	 strcat(conninfo , " options='-O -c gp_session_role=utility'");
	 
	 printf("%s running.\n", myname);

	/* Make a connection to the database */
	conn = connect_to_db(conninfo, force_password);
	
	res = PQexec(conn, "SELECT VERSION()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "SELECT VERSION() failed: %s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		fprintf(stderr," UPGRADE FAILED\n");
		exit(1);
	}

	version = PQgetvalue(res, 0, 0);
	
	if (strchr(version,')') != NULL)
	{
		char *ptr = strchr(version,')') + 1;
		*ptr = '\0';
	}

	printf("%s:  Version of system:  %s\n", myname, version);

	if (strstr(version,"Greenplum Database") != 0) 
		isMPP = 1;
	else
		isMPP = 0;
		
	if (!isMPP)
	{	
		fprintf(stderr, "This is not a Greenplum Database system");
		PQfinish(conn);
		exit(1);
	}

	if (strncmp(version, "PostgreSQL 8.2.4",strlen("PostgreSQL 8.2.4")) != 0)
	{
		fprintf(stderr,"You must upgrade the Greenplum Database software to Greenplum Database 3.1\n"
				"and restart the system, before running this utility\n");
		PQfinish(conn);
		exit(1);
		
	}
	
	PQclear(res);
	
	res = PQexec(conn, "select datname from pg_catalog.pg_database order by oid");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "select datname from pg_database: command failed: %s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		fprintf(stderr," UPGRADE FAILED\n");
		exit(1);
	}
	
	databasenames = malloc(sizeof(char *) * PQntuples(res));
	databasecount = PQntuples(res);
	for (i = 0; i < databasecount; i++)
	{
		databasenames[i] = strdup(PQgetvalue(res, i, 0));
	}
	PQclear(res);

	res = PQexec(conn, "select * from pg_catalog.gp_configuration where "
				 "dbid = 1 or content >= 0 order by content," 
				 "definedprimary desc, dbid");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "select * from gp_configuration: command failed: %s",
				PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
   		exit(1);
	}

	segdbs = malloc(sizeof(config_t) * PQntuples(res));
	segdbcount = PQntuples(res);


	/* next, save the rows */
	for (i = 0; i < segdbcount; i++)
	{
		segdbs[i].content = atol(PQgetvalue(res, i, 0));
		segdbs[i].definedprimary = PQgetvalue(res, i, 1)[0] == 't';
		segdbs[i].dbid = atol(PQgetvalue(res, i, 2));
		segdbs[i].isprimary = PQgetvalue(res, i, 3)[0] == 't';
		segdbs[i].valid = PQgetvalue(res, i, 4)[0] == 't';
		if (i == 0 && segdbs[i].content == -1 && host != NULL)
			segdbs[i].hostname = host;
		else 
			segdbs[i].hostname = strdup(PQgetvalue(res, i, 5));
		segdbs[i].port = strdup(PQgetvalue(res, i, 6));
	}
	
	
	PQclear(res);
	
	segdbs[0].conn = conn;
	
	printf("gp_upgrade:  Got configuration information from entry db.\n");

	for (d = 0; d < databasecount; d++)
	{
		printf("gp_upgrade:  Upgrading database %s\n",databasenames[d]);
		
		for (i = 0; i < segdbcount; i++)
		{
			char conninfo[1024];
			sprintf(conninfo,"host=%s port=%s dbname=%s "
					"options='-O -c gp_session_role=utility' ",
					segdbs[i].hostname, segdbs[i].port, databasenames[d]);
	
			/* Make a connection to the database */
		   	segdbs[i].conn = connect_to_db(conninfo, false);
		   
		   	if (d == 0)
		   	{
		   		if (verbose)
					printf("\nupdating segdb content %d definedprimary %d dbid %d\n\n",segdbs[i].content,segdbs[i].definedprimary,segdbs[i].dbid);
		   		
		   		run_sql_command(segdbs[i].conn, "update pg_database set "
								"datallowconn = 't' where datname='template0'");
		   	}
		}
		
		/*
		 * We now have a connection open to the database on every segDB 
		 * (0 is masterDB)
		 */
		
		for (i = 0; i < segdbcount; i++)
		{
			conn = segdbs[i].conn;

			if (verbose)
				printf("\nupdating segdb content %d definedprimary %d dbid "
					   "%d for database %s\n\n",segdbs[i].content,
					   segdbs[i].definedprimary,segdbs[i].dbid,
					   databasenames[d]);
			
			if (d==0)
		  	{
		  		run_sql_command(conn, "VACUUM FULL pg_authid");
		  	}
			
			if (strcmp(databasenames[d],"template1")==0 ||
				strcmp(databasenames[d],"template0")==0)
			{
				run_sql_command(conn, "VACUUM FULL");
				run_sql_command(conn, "VACUUM FREEZE");
			}
		}
	}
	
	for (i = 0; i < segdbcount; i++)
	{ 	
		if (verbose)
			printf("\nupdating segdb content %d definedprimary %d dbid %d\n\n",
				   segdbs[i].content,segdbs[i].definedprimary,segdbs[i].dbid);
	
		conn = segdbs[i].conn;
		run_sql_command(conn, "update pg_database set datallowconn = 'f' "
						"where datname='template0'");
	}
	
	printf("Done\n");
	/* close the connection to the database and cleanup */
	PQfinish(conn);
	return(0);	
}
