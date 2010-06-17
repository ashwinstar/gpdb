#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "gpmonlib.h"
#include "gpmondb.h"
#include "libpq-fe.h"
#include "apr_strings.h"
#include "apr_file_io.h"
#include "time.h"

static const char* gpdb_exec(PGconn** pconn, PGresult** pres, const char* query)
{
	const char *connstr = "dbname='" GPMON_DB "' user='" GPMON_DBUSER
	"' connect_timeout='30'";
	PGconn *conn = NULL;
	PGresult* res = 0;
	ExecStatusType status;

	conn = PQconnectdb(connstr);
	if (PQstatus(conn) != CONNECTION_OK)
		return PQerrorMessage(conn);

	TR1(("Query: %s\n", query));

	*pconn = conn;
	res = PQexec(conn, query);
	status = PQresultStatus(res);
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
		return PQerrorMessage(conn);

	*pres = res;
	return 0;
}

int gpdb_exec_search_for_at_least_one_row(const char* QUERY)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	int rowcount;
	const char* errmsg = gpdb_exec(&conn, &result, QUERY);
	int res = 0;
	if (errmsg)
	{
		gpmon_warning(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
	}
	else
	{
		rowcount = PQntuples(result);
		if (rowcount > 0)
			res = 1;
	}

	PQclear(result);
	PQfinish(conn);

	return res;
}

int gpdb_validate_gpperfmon(void)
{
	/* Check db */
	if (!gpdb_gpperfmon_db_exists())
		return 0;

	/* check post */
	if (!gpdb_get_gpmon_port())
		return 0;

	/* check external tables are accessable by gpmon user */
	if (!gpdb_validate_ext_table_access())
		return 0;

	return 1;
}

int gpdb_gpperfmon_db_exists(void)
{
	const char *connstr = "dbname='" GPMON_DB "' user='" GPMON_DBUSER
	"' connect_timeout='30'";
	PGconn *conn = NULL;
	int db_exists = 0;

	conn = PQconnectdb(connstr);
	if (PQstatus(conn) == CONNECTION_OK)
	{
		db_exists = 1;
		PQfinish(conn);
	}

	return db_exists;
}

int gpdb_gpperfmon_enabled(void)
{
	const char* QUERY = "SELECT 1 FROM pg_settings WHERE name = 'gp_enable_gpperfmon' and setting='on'";
	return gpdb_exec_search_for_at_least_one_row(QUERY);
}

int gpdb_validate_ext_table_access(void)
{
	const char* QUERY = "select * from master_data_dir";
	return gpdb_exec_search_for_at_least_one_row(QUERY);
}

int gpdb_get_gpmon_port(void)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	char* curport = 0;
	int rowcount;
	const char* QUERY =
			"SELECT setting FROM pg_settings WHERE name = 'gpperfmon_port'";
	int port = 0;
	const char* errmsg = gpdb_exec(&conn, &result, QUERY);
	if (errmsg)
	{
		gpmon_warning(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
	}
	else
	{
		rowcount = PQntuples(result);
		if (rowcount > 0)
		{
			curport = PQgetvalue(result, 0, 0);
			if (curport)
			{
				port = atoi(curport);
			}
		}
	}
	PQclear(result);
	PQfinish(conn);

	if (!port)
	{
		gpmon_warning(FLINE, "Unable to retrieve gpperfmon_port GUC from GPDB\n");
	}
	return port;
}

void gpdb_get_hostlist(int* hostcnt, char*** hostvec, char*** datadirvec,
		apr_pool_t* pool)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	int rowcount, i;
	const char
			* QUERY = "SELECT hostname, fselocation as datadir, dbid FROM pg_filespace_entry JOIN gp_segment_configuration on (dbid = fsedbid) WHERE fsefsoid = (select oid from pg_filespace where fsname='pg_system') AND mode = 's' AND STATUS = 'u' ORDER BY dbid ASC";
	int cnt = 0;
	char** vec = 0;
	char** vec2 = 0;
	const char* errmsg = gpdb_exec(&conn, &result, QUERY);
	if (errmsg)
	{
		gpmon_warning(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
		*hostcnt = 0;
		*hostvec = NULL;
		*datadirvec = NULL;
	}
	else
	{
		rowcount = PQntuples(result);
		if (rowcount > 0)
		{
			vec = apr_pcalloc(pool, sizeof(char*) * (1 + rowcount));
			CHECKMEM(vec);
			vec2 = apr_pcalloc(pool, sizeof(char*) * (1 + rowcount));
			CHECKMEM(vec2);

			for (i = 0; i < rowcount; i++)
			{
				char* p = PQgetvalue(result, i, 0);
				char* datadir = PQgetvalue(result, i, 1);
				if (!p || !datadir)
					continue;
				p = apr_pstrdup(pool, p);
				CHECKMEM(p);
				datadir = apr_pstrdup(pool, datadir);
				CHECKMEM(datadir);
				p = gpmon_trim(p);
				datadir = gpmon_trim(datadir);
				vec[cnt] = p;
				vec2[cnt++] = datadir;
			}
		}
		*hostcnt = cnt;
		*hostvec = vec;
		*datadirvec = vec2;
	}

	PQclear(result);
	PQfinish(conn);
}

void gpdb_get_master_data_dir(char** hostname, char** mstrdir, apr_pool_t* pool)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	const char* QUERY = "select * from master_data_dir";
	char* dir = 0;
	char* hname = 0;
	int rowcount;
	const char* errmsg = gpdb_exec(&conn, &result, QUERY);
	if (errmsg)
	{
		gpmon_warning(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
	}
	else
	{
		rowcount = PQntuples(result);
		if (rowcount > 0)
		{
			hname = PQgetvalue(result, 0, 0);
			dir = PQgetvalue(result, 0, 1);
		}

		if (!hname || !dir)
		{
			gpmon_warning(FLINE, "unable to get master data directory");
		}
		else
		{
			hname = apr_pstrdup(pool, gpmon_trim(hname));
			CHECKMEM(hname);

			dir = apr_pstrdup(pool, gpmon_trim(dir));
			CHECKMEM(dir);
		}
	}

	PQclear(result);
	PQfinish(conn);

	*hostname = hname;
	*mstrdir = dir;
}

static apr_status_t check_partition(const char* tbl, apr_pool_t* pool)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	const char* errmsg;
	const int QRYBUFSIZ = 1024;
	struct tm tm;
	time_t now;
	char qry[QRYBUFSIZ];
	const char* QRYFMT = "alter table %s_history add partition start ('%d-%02d-01 00:00:00'::timestamp without time zone) inclusive end ('%d-%02d-01 00:00:00'::timestamp without time zone) exclusive;";

	unsigned short year[3];
	unsigned char month[3];

	TR0(("check partitions on %s_history\n", tbl));

   	now = time(NULL);
	if (!gmtime_r(&now, &tm))
	{
		gpmon_warning(FLINE, "error in check_partition getting current time\n");
		return APR_EGENERAL;
	}

	year[0] = 1900 + tm.tm_year;
	month[0] = tm.tm_mon+1;

	if (year[0] < 1 || month[0] < 1 || year[0] > 2030 || month[0] > 12)
	{
		gpmon_warning(FLINE, "invalid current month/year in check_partition %u/%u\n", month, year);
		return APR_EGENERAL;
	}

	if (month[0] < 11)
	{
		month[1] = month[0] + 1;
		month[2] = month[0] + 2;

		year[1] = year[0];
		year[2] = year[0];
	}
	else if (month[0] == 11)
	{
		month[1] = 12;
		month[2] = 1;

		year[1] = year[0];
		year[2] = year[0] + 1;
	}
	else 
	{
		month[1] = 1;
		month[2] = 2;

		year[1] = year[0] + 1;
		year[2] = year[0] + 1;
	}

	snprintf(qry, QRYBUFSIZ, QRYFMT, tbl, year[0], month[0], year[1], month[1]);
	TR0(("%s\n", qry));
	errmsg = gpdb_exec(&conn, &result, qry);
	if (errmsg)
	{
		TR1(("partion add response from server: %s\n", errmsg));
	}
	PQclear(result);
	PQfinish(conn);
	conn = 0;
	result = 0;

	snprintf(qry, QRYBUFSIZ, QRYFMT, tbl, year[1], month[1], year[2], month[2]);
	TR0(("%s\n", qry));
	errmsg = gpdb_exec(&conn, &result, qry);
	if (errmsg)
	{
		TR1(("partion add response from server: %s\n", errmsg));
	}
	PQclear(result);
	PQfinish(conn);

	TR0(("check partitions on %s_history done\n", tbl));
	return APR_SUCCESS;
}

static apr_status_t harvest(const char* tbl, apr_pool_t* pool)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	const int QRYBUFSIZ = 1024;
	char qrybuf[QRYBUFSIZ];
	const char* QRYFMT = "insert into %s_history select * from _%s_tail;";
	const char* errmsg;
	apr_status_t res = APR_SUCCESS;

	snprintf(qrybuf, QRYBUFSIZ, QRYFMT, tbl, tbl);

	errmsg = gpdb_exec(&conn, &result, qrybuf);
	if (errmsg)
	{
		res = 1;
		gpmon_warningx(FLINE, 0, "---- HARVEST %s FAILED ---- on query %s with error %s\n", tbl, qrybuf, errmsg);
	}
	else
	{
		TR1(("load completed OK: %s\n", tbl));
	}

	PQclear(result);
	PQfinish(conn);
	return res;
}


apr_status_t truncate_file(char* fn, apr_pool_t* pool)
{
	apr_file_t *fp = NULL;
	apr_status_t status;

	status = apr_file_open(&fp, fn, APR_WRITE|APR_CREATE|APR_TRUNCATE, APR_UREAD|APR_UWRITE, pool);

	if (status == APR_SUCCESS)
	{
		status = apr_file_trunc(fp, 0);
		apr_file_close(fp);
	}

	if (status != APR_SUCCESS)
	{
		gpmon_warningx(FLINE, 0, "harvest process truncate file %s failed", fn);
	}
	else
	{
		TR1(("harvest truncated file %s: ok\n", fn));
	}

	return status;
}


/* rename tail to stage */
static apr_status_t rename_tail_files(const char* tbl, apr_pool_t* pool)
{
	char srcfn[PATH_MAX];
	char dstfn[PATH_MAX];

	apr_status_t status;

	/* make the file names */
	snprintf(srcfn, PATH_MAX, "%s%s_tail.dat", GPMON_DIR, tbl);
	snprintf(dstfn, PATH_MAX, "%s%s_stage.dat", GPMON_DIR, tbl);

	status = apr_file_rename(srcfn, dstfn, pool);
	if (status != APR_SUCCESS)
	{
		gpmon_warningx(FLINE, status, "harvest failed renaming %s to %s", srcfn, dstfn);
		return status;
	}
	else
	{
		TR1(("harvest rename %s to %s success\n", srcfn, dstfn));
	}

	return status;
}

/* append stage data to _tail file */
static apr_status_t append_to_harvest(const char* tbl, apr_pool_t* pool)
{
	char srcfn[PATH_MAX];
	char dstfn[PATH_MAX];

	apr_status_t status;

	/* make the file names */
	snprintf(srcfn, PATH_MAX, "%s%s_stage.dat", GPMON_DIR, tbl);
	snprintf(dstfn, PATH_MAX, "%s_%s_tail.dat", GPMON_DIR, tbl);

	status = apr_file_append(srcfn, dstfn, APR_FILE_SOURCE_PERMS, pool);
	if (status != APR_SUCCESS)
	{
		gpmon_warningx(FLINE, status, "harvest failed appending %s to %s", srcfn, dstfn);
	}
	else
	{
		TR1(("harvest append %s to %s: ok\n", srcfn, dstfn));
	}

	return status;
}

typedef apr_status_t eachtablefunc(const char* tbl, apr_pool_t*);

char* all_tables[] = { "system", "queries", "iterators", "database", "segment" };

apr_status_t call_for_each_table(eachtablefunc func, apr_pool_t* pool)
{ 
 	apr_status_t status = APR_SUCCESS;
	apr_status_t r;
	int num_tables = sizeof(all_tables) / sizeof (char*);
	int i;

	for (i = 0; i < num_tables; ++i)
	{
		r = func(all_tables[i], pool);
		if (r != APR_SUCCESS)
		{
			status = r;
		}
	}
	
	return status;
}

/* rename tail files to stage files */
apr_status_t gpdb_rename_tail_files(apr_pool_t* pool)
{
	return call_for_each_table(rename_tail_files, pool);
}

/* copy data from stage files to harvest files */
apr_status_t gpdb_copy_stage_to_harvest_files(apr_pool_t* pool)
{
	return call_for_each_table(append_to_harvest, pool);
}

/* truncate _tail files */
apr_status_t empty_harvest_file(const char* tbl, apr_pool_t* pool)
{
	char fn[PATH_MAX];
	snprintf(fn, PATH_MAX, "%s_%s_tail.dat", GPMON_DIR, tbl);
	return truncate_file(fn, pool);
}

/* truncate tail files */
apr_status_t truncate_tail_file(const char* tbl, apr_pool_t* pool)
{
	char fn[PATH_MAX];
	snprintf(fn, PATH_MAX, "%s%s_tail.dat", GPMON_DIR, tbl);
	return truncate_file(fn, pool);
}

/* truncate _tail files to clear data already loaded into the DB */
apr_status_t gpdb_truncate_tail_files(apr_pool_t* pool)
{
	return call_for_each_table(truncate_tail_file, pool);
}

/* truncate _tail files to clear data already loaded into the DB */
apr_status_t gpdb_empty_harvest_files(apr_pool_t* pool)
{
	return call_for_each_table(empty_harvest_file, pool);
}

/* insert _tail data into history table */
apr_status_t gpdb_harvest(void)
{
	return call_for_each_table(harvest, NULL);
}

/* insert _tail data into history table */
apr_status_t gpdb_check_partitions(void)
{
	return call_for_each_table(check_partition, NULL);
}

