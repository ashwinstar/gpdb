#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

#include "postgres.h"
#include "postgres_fe.h"
#include "libpq-fe.h"
#include "libpq/pqsignal.h"
#include "gpupgrade_cmds.h"
#include "gpdowngrade_cmds.h"
#include "catalog/pg_class.h"
#include "catalog/pg_exttable.h"
#include "catalog/pg_window.h"

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
static PGresult *run_query(PGconn *conn, const char *str, char *host);

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
 * To downgrade the 3.1 schema changes (addition of columns to pg_exttable
 * and pg_window), we must drop the table and recreate it because
 * alter table drop column simply work. This is because the routines which
 * interact with those tables (particularly pg_exttable) expect a layout
 * which matches their Form_pg_ schema. The dropped columns skew this
 * and so the mapping cannot be made.
 *
 * With pg_exttable we have to be careful insert the rows that we remove.
 *
 * We also need to remove internal dependencies on the system catalogs in order
 * to successfully drop the table... we must re-add these too of course.
 */
static void
downgrade_catalog_schema(config_t *conf)
{
	PGresult *exttableres;
#if 0
	PGresult *dependres;
#endif
	char query[1024];
	int i;
	
	/* Drop pg_exttab */
#if 0
	snprintf(query, sizeof(query), "DELETE FROM pg_depend where "
			 "refobjid = '%u' returning *;", ExtTableRelationId);
	dependres = run_query(conf->conn, query, conf->hostname);
#endif
	exttableres = run_query(conf->conn, "select reloid, location, fmttype, "
							"fmtopts, command from pg_exttable;",
							conf->hostname);
	
#if 0
	run_query(conf->conn, "drop index pg_exttable_reloid_index;",
			  conf->hostname);
#endif
#if 0
	run_query(conf->conn, "DROP TABLE pg_exttable;", conf->hostname);
	snprintf(query, sizeof(query), "select gp_forget_relation('%u');",
			 ExtTableRelationId);
	run_query(conf->conn, query, conf->hostname);

	/* by screwing with the catalogs, we don't get the type dropped */
	run_query(conf->conn, "delete from pg_type where typname = 'pg_exttable' "
			  "and typnamespace = '11';", conf->hostname);
#endif
	snprintf(query, sizeof(query), "delete from pg_attribute where "
			 "attnum > 5 AND attrelid = '%u';", ExtTableRelationId);
	run_query(conf->conn, query, conf->hostname);
	snprintf(query, sizeof(query), "update pg_class set relnatts = '5' "
			 "WHERE oid = '%u'", ExtTableRelationId);
	run_query(conf->conn, query, conf->hostname);
	run_query(conf->conn, "truncate pg_catalog.pg_exttable;",
			  conf->hostname);
	/* add the rows */
	for (i = 0; i < PQntuples(exttableres); i++)
	{
		snprintf(query, sizeof(query), "INSERT INTO pg_catalog.pg_exttable "
				  "(reloid, location, fmttype, fmtopts, command) "
				  "VALUES('%s', '%s', '%s', '%s', '%s');",
				  PQgetvalue(exttableres, i, 0), PQgetvalue(exttableres, i, 1),
				  PQgetvalue(exttableres, i, 2), PQgetvalue(exttableres, i, 3),
				  PQgetvalue(exttableres, i, 4));
		run_query(conf->conn, query, conf->hostname);
	}

	PQclear(exttableres);

	snprintf(query, sizeof(query), "delete from pg_attribute where "
			 "attnum > 9 AND attrelid = '%u';", WindowRelationId);
	run_query(conf->conn, query, conf->hostname);
	snprintf(query, sizeof(query), "update pg_class set relnatts = '9' "
			 "WHERE oid = '%u'", WindowRelationId);
	run_query(conf->conn, query, conf->hostname);
	run_query(conf->conn, "truncate pg_catalog.pg_window;",
			  conf->hostname);
#if 0
	/* invalidate the catalog cache */
	run_query(conf->conn, "select gp_invalidate_syscache();", conf->hostname);
	/* pg_window is easier, just drop and recreate */
	snprintf(query, sizeof(query), "DELETE FROM pg_depend where "
			 "refobjid = '%u' returning *;", WindowRelationId);
	dependres = run_query(conf->conn, query, conf->hostname);
#endif

#if 0
	run_query(conf->conn, "drop index pg_window_fnoid_index;",
			  conf->hostname);
	run_query(conf->conn, "drop table pg_catalog.pg_window;", conf->hostname);
	snprintf(query, sizeof(query), "select gp_forget_relation('%u');",
			 WindowRelationId);
	run_query(conf->conn, query, conf->hostname);
	/* read above for why we do this */
	run_query(conf->conn, "delete from pg_type where typname = 'pg_window' "
			  "and typnamespace = '11';", conf->hostname);
	
#endif
#if 0
	/* start a new txn */
	run_query(conf->conn, "COMMIT; BEGIN;", conf->hostname);
	
	run_query(conf->conn, "CREATE TABLE pg_catalog.pg_exttable"
			  "(reloid oid not null, location text[], fmttype \"char\", "
			  "fmtopts text, command text);", conf->hostname);
#endif
	
#if 0
	/* add the dependency */
	snprintf(query, sizeof(query), "INSERT INTO pg_catalog.pg_depend "
			  "(classid, objid, objsubid, refclassid, refobjid, refobjsubid, "
			  "deptype) VALUES('%s', '%s', '%s', '%s', '%s', '%s', '%s');",
			  PQgetvalue(dependres, i, 0), PQgetvalue(dependres, i, 1),
			  PQgetvalue(dependres, i, 2), PQgetvalue(dependres, i, 3),
			  PQgetvalue(dependres, i, 4), PQgetvalue(dependres, i, 5),
			  PQgetvalue(dependres, i, 6));
	run_query(conf->conn, query, conf->hostname);

	PQclear(dependres);
#endif
#if 0
	/* invalidate the catalog cache */
	run_query(conf->conn, "select gp_invalidate_syscache();", conf->hostname);
	run_query(conf->conn, "create table pg_catalog.pg_window ("
			  "winfnoid regproc not null, winrequireorder boolean, "
			  "winallowframe boolean, winpeercount boolean, "
			  "wincount boolean, winfunc regproc, winprefunc regproc, "
			  "winpretype oid, winfinfunc regproc);", conf->hostname);

#endif
#if 0
	snprintf(query, sizeof(query), "SELECT oid, "
			 "gp_oid_update(oid, %u, ctid, %u) "
			 "FROM pg_catalog.pg_class WHERE relname = 'pg_exttable' AND "
			 "relnamespace = '11'", ExtTableRelationId, RelationRelationId);
	res = run_query(conf->conn, query, conf->hostname);

	snprintf(query, sizeof(query), "select gp_forget_relation('%s');",
			 PQgetvalue(res, 0, 0));
	run_query(conf->conn, query, conf->hostname);
	
	/* invalidate the catalog cache */
	run_query(conf->conn, "select gp_invalidate_syscache();", conf->hostname);
	/* update pg_attribute and pg_type references */
	snprintf(query, sizeof(query), "UPDATE pg_catalog.pg_attribute "
			 "SET attrelid = '%u' WHERE attrelid = '%s'",
			 ExtTableRelationId, PQgetvalue(res, 0, 0));
	run_query(conf->conn, query, conf->hostname);
	
	snprintf(query, sizeof(query), "UPDATE pg_catalog.pg_type "
			 "SET typrelid = '%u' WHERE typrelid = '%s'",
			 ExtTableRelationId, PQgetvalue(res, 0, 0));
	run_query(conf->conn, query, conf->hostname);
	/* now pg_window */
	snprintf(query, sizeof(query), "SELECT oid, "
			 "gp_oid_update(oid, %u, ctid, %u) "
			 "FROM pg_class WHERE relname = 'pg_window' AND "
			 "relnamespace = '11'", WindowRelationId, RelationRelationId);

	res = run_query(conf->conn, query, conf->hostname);
	snprintf(query, sizeof(query), "select gp_forget_relation('%s');",
			 PQgetvalue(res, 0, 0));
	run_query(conf->conn, query, conf->hostname);

	/* update pg_attribute and pg_type references */
	snprintf(query, sizeof(query), "UPDATE pg_catalog.pg_attribute "
			 "SET attrelid = '%u' WHERE attrelid = '%s'",
			 WindowRelationId, PQgetvalue(res, 0, 0));
	run_query(conf->conn, query, conf->hostname);
	snprintf(query, sizeof(query), "UPDATE pg_catalog.pg_type "
			 "SET typrelid = '%u' WHERE typrelid = '%s'",
			 WindowRelationId, PQgetvalue(res, 0, 0));
	run_query(conf->conn, query, conf->hostname);
	
	/* invalidate the catalog cache */
	run_query(conf->conn, "select gp_invalidate_syscache();", conf->hostname);

#endif
	/*
	run_query(conf->conn, "REVOKE ALL ON TABLE pg_catalog.pg_exttable "
			 " FROM PUBLIC;"
			 " GRANT SELECT ON TABLE pg_catalog.pg_exttable TO PUBLIC;",
			 conf->hostname);
	run_query(conf->conn, "REVOKE ALL ON TABLE pg_catalog.pg_window "
			 " FROM PUBLIC;"
			 " GRANT SELECT ON TABLE pg_catalog.pg_window TO PUBLIC;",
			 conf->hostname);
	run_query(conf->conn, "reindex table pg_class;", conf->hostname);
	run_query(conf->conn, "select gp_invalidate_syscache();", conf->hostname);

	run_query(conf->conn, "create unique index pg_exttable_reloid_index "
			  "on pg_catalog.pg_exttable(reloid);", conf->hostname);
	run_query(conf->conn, "create unique index pg_window_fnoid_index "
			  "on pg_catalog.pg_window(winfnoid);", conf->hostname);
	*/
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
		/* XXX: must be in this order */
		run_query(conf->conn, "alter table pg_catalog.pg_exttable "
				  "add column rejectlimit int;", conf->hostname);
		run_query(conf->conn, "alter table pg_catalog.pg_exttable "
				  "add column rejectlimittype \"char\";", conf->hostname);
		run_query(conf->conn, "alter table pg_catalog.pg_exttable "
				  "add column fmterrtbl oid;", conf->hostname);

		/* pg_window */
		run_query(conf->conn, "alter table pg_catalog.pg_window "
				  "add column winkind \"char\";", conf->hostname);

		run_query(conf->conn, "create or replace function "
				  "gp_oid_update(oid, oid, tid, oid) returns bool "
				  "as '$libdir/libgpupgradefuncs', 'gp_oid_update' "
				  "LANGUAGE C VOLATILE STRICT;", conf->hostname);

		/* 
		 * We create the function name as gp_str2node2str() so that we do
		 * not have to special case for upgrade or downgrade. See downgrade
		 * case below.
		 */
		run_query(conf->conn, "create or replace function "
				  "gp_str2node2str(text) returns text as "
				  "'$libdir/libgpupgradefuncs.so', 'gp_str30_2node2str' "
				  "language c strict volatile;", conf->hostname);
}
	else
	{
		/* downgrade case */
//		mylog(UERROR, "DOWNGRADE NOT IMPLEMENTED!\n");
//		exit(1);

		
		run_query(conf->conn, "create or replace function "
				  "gp_oid_update(oid, oid, tid, oid) returns bool "
				  "as '$libdir/libgpupgradefuncs', 'gp_oid_update' "
				  "LANGUAGE C VOLATILE STRICT;", conf->hostname);

		run_query(conf->conn, "create or replace function "
				  "gp_str2node2str(text) returns text as "
				  "'$libdir/libgpupgradefuncs.so', 'gp_str31_2node2str'"
				  "language c strict volatile;", conf->hostname);

		run_query(conf->conn, "create or replace function "
				  "gp_invalidate_syscache() returns bool as "
				  "'$libdir/libgpupgradefuncs.so', 'gp_invalidate_syscache'"
				  "language c strict volatile;", conf->hostname);

		run_query(conf->conn, "create or replace function "
				  "gp_forget_relation(oid) returns bool as "
				  "'$libdir/libgpupgradefuncs.so', 'gp_forget_relation'"
				  "language c strict volatile;", conf->hostname);
		run_query(conf->conn, "create or replace function "
				  "gp_downgrade_controlfile() returns bool as "
				  "'$libdir/libgpupgradefuncs.so', 'gp_downgrade_controlfile'"
				  "language c strict volatile;", conf->hostname);
		downgrade_catalog_schema(conf);
	}

}



static void
run_upgrade_commands(config_t *conf)
{
	uint16 i;
	const char **cmds;
	const char *reindex_tables[] = { "pg_type", "pg_proc", "pg_operator",
								   "pg_opclass", "pg_attribute", "pg_amop",
								   "pg_amproc", /*"pg_window", "pg_exttable", */
								   NULL};
	char query[1024];

	/* update serialised strings */
	run_query(conf->conn, "update pg_rewrite set ev_action = "
			  "gp_str2node2str(ev_action), ev_qual = gp_str2node2str(ev_qual);",
			  conf->hostname);
	run_query(conf->conn, "update pg_type set typdefaultbin = "
			  "gp_str2node2str(typdefaultbin) WHERE "
			  "typdefaultbin NOTNULL;", conf->hostname);
	run_query(conf->conn, "update pg_constraint set conbin = "
			  "gp_str2node2str(conbin) WHERE conbin NOTNULL;",
			  conf->hostname);
	run_query(conf->conn, "update pg_index set indexprs = "
			  "gp_str2node2str(indexprs), indpred = gp_str2node2str(indpred) "
			  "WHERE indexprs NOTNULL or indpred NOTNULL;",
			  conf->hostname);

//	Assert((sizeof(upgrade_cmds) / sizeof(char *)) < sizeof(uint16) * 8);

	if (upgrade)
		cmds = upgrade_cmds;
	else
		cmds = downgrade_cmds;

	for (i = 0; cmds[i]; i++)
	{
		const char *cmd = cmds[i];

		run_query(conf->conn, cmd, conf->hostname);

	}

	/* reindex the affected tables */
	for (i = 0; reindex_tables[i]; i++)
	{
		char qry[128];
		snprintf(qry, sizeof(qry), "reindex table pg_catalog.\"%s\";",
				 reindex_tables[i]);
		run_query(conf->conn, qry, conf->hostname);
	}

	/* update attributes using 64 bit types */
	snprintf(query, sizeof(query),
			 "update pg_attribute set attbyval = '%c' where "
			  "attbyval != '%c' AND atttypid "
			  "in('float4'::regtype, 'float8'::regtype, 'int8'::regtype, "
			  "'time'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype);",
			  upgrade ? 't' : 'f', upgrade ? 't' : 'f');
	run_query(conf->conn, query, conf->hostname);
}

static void
post_upgrade(config_t *conf)
{
	run_query(conf->conn, "drop function gp_oid_update(oid, oid, tid, oid);",
			  conf->hostname);
	run_query(conf->conn, "drop function gp_str2node2str(text);", 
			  conf->hostname);
	if (!upgrade)
		run_query(conf->conn, "drop function gp_invalidate_syscache();",
				  conf->hostname);
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
run_query(PGconn *conn, const char *str, char *host)
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
	PQclear(res);
	res = run_query(conn, "SET SEARCH_PATH = 'pg_catalog'", conninfo);
	PQclear(res);
	/* we corrupt some indexes doing the upgrade, so turn off index scan */
	res = run_query(conn, "set enable_indexscan to off;", conninfo);
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

	while ((c = getopt_long(argc, argv, "h:p:U:dD?tVW",
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
	run_query(conn, "abort;", conninfo);
	
	mylog(UDEBUG, "got configuration information from entry db.");

	mylog(UDEBUG, "opening template0 databases.");
	
	/* allow access to template0 */
	for (i = 0; i < segdbcount; i++)
	{
		config_t *conf = segdbs[i];
		char connstr[128];
		
		if (strcmp(conf->dbname, "template1") != 0)
			continue;
		
		mylog(UDEBUG, "updating segdb content %d definedprimary %d dbid %d",
			  conf->content,conf->definedprimary,conf->dbid);

		sprintf(connstr,"host=%s port=%s dbname=%s " CONNOPTIONS,
				conf->hostname, conf->port, conf->dbname);
	
		/* Make a connection to the database */
	   	conf->conn = connect_to_db(connstr, false);

		conf->conninfo = strdup(connstr);
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
		char connstr[1024];
		
		mylog(UINFO, "upgrading segment %s:%s (%s)", conf->hostname,
			   conf->port, conf->dbname);

		if (!conf->conn)
		{
			sprintf(connstr,"host=%s port=%s dbname=%s " CONNOPTIONS,
					conf->hostname, conf->port, conf->dbname);
	
			/* Make a connection to the database */
		   	conf->conn = connect_to_db(connstr, false);
			conf->conninfo = strdup(connstr);
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
				   "%d for database %s, connstr = \"%s\"",segdbs[i]->content,
				   segdbs[i]->definedprimary, segdbs[i]->dbid,
				   segdbs[i]->dbname, segdbs[i]->conninfo);

		/* modify schema */
		schema_upgrade(segdbs[i]);
		check_ok(segdbs);

		/* run the upgrade command */
		run_upgrade_commands(segdbs[i]);
		check_ok(segdbs);		
		post_upgrade(segdbs[i]);
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
			run_query(segdbs[i]->conn, "VACUUM FULL", segdbs[i]->hostname);
			run_query(segdbs[i]->conn, "VACUUM FREEZE", segdbs[i]->hostname);
		}
		if (!upgrade)
		{
			run_query(segdbs[i]->conn, "select gp_downgrade_controlfile();",
					  segdbs[i]->hostname);
			run_query(segdbs[i]->conn, "drop function "
					  "gp_downgrade_controlfile();", segdbs[i]->hostname);

		}
		PQfinish(segdbs[i]->conn);
	}
	printf("Done\n");
	/* close the connection to the database and cleanup */
	return(0);	
}
