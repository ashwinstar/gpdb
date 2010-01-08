#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "gpmonlib.h"
#include "gpmondb.h"
#include "libpq-fe.h"
#include "apr_strings.h"

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
	int enabled = 0;

	PGconn* conn = 0;
	PGresult* result = 0;
	ExecStatusType status;

	int rowcount;

	const char *connstr = "dbname='template1' user='" GPMON_DBUSER
			"' connect_timeout='30'";

	const char* query =
			"SELECT 1 FROM pg_settings WHERE name = 'gp_enable_gpperfmon' and setting='on'";

	conn = PQconnectdb(connstr);
	if (PQstatus(conn) == CONNECTION_OK)
	{
		result = PQexec(conn, query);
		status = PQresultStatus(result);
		if (status == PGRES_COMMAND_OK && status == PGRES_TUPLES_OK)
		{
			rowcount = PQntuples(result);
			if (rowcount > 0)
				enabled = 1;
		}
	}

	PQclear(result);
	PQfinish(conn);

	return enabled;
}

int gpdb_validate_ext_table_access(void)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	const char* QUERY = "select * from master_data_dir";
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
			* QUERY =
					"SELECT hostname, datadir, dbid FROM gp_configuration WHERE valid = 't' ORDER BY dbid ASC";
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

void gpdb_get_config_md5(unsigned char md5[APR_MD5_DIGESTSIZE])
{
	PGconn* conn = 0;
	PGresult* result = 0;
	const char
			* QUERY =
					"select content || case when definedprimary then 1 else 0 end || dbid || case when isprimary then 1 else 0 end || case when valid then 1 else 0 end || hostname || port || datadir from gp_configuration";

	apr_md5_ctx_t ctx;
	int e, i;
	int rowcount;
	const char* errmsg = gpdb_exec(&conn, &result, QUERY);
	if (errmsg)
	{
		gpmon_warning(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
		memset(md5, 0, APR_MD5_DIGESTSIZE);
	}
	else
	{
		if (0 != (e = apr_md5_init(&ctx)))
		{
			gpmon_warningx(FLINE, e, "apr_md5_init failed");
		}

		rowcount = PQntuples(result);
		for (i = 0; i < rowcount; i++)
		{
			const char* p = PQgetvalue(result, i, 0);
			if (p)
			{
				if (0 != (e = apr_md5_update(&ctx, p, strlen(p))))
					gpmon_warningx(FLINE, e, "apr_md5_update failed");
			}
		}
		apr_md5_final(md5, &ctx);
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

static apr_status_t do_rename(const char* tbl)
{
	char src[200];
	char dst[200];
	FILE* fp;

	/* make the file names */
	sprintf(src, "%s%s_tail.dat", GPMON_DIR, tbl);
	sprintf(dst, "%s_%s_tail.dat", GPMON_DIR, tbl);

	/* delete destination */
	unlink(dst);

	/* rename src -> dst */
	if (0 != rename(src, dst))
		return APR_EINVAL;

	/* recreate src as empty file */
	fp = fopen(src, "a");
	if (fp)
		fclose(fp);

	return 0;
}

static apr_status_t harvest(const char* tbl)
{
	PGconn* conn = 0;
	PGresult* result = 0;
	char qrybuf[1024];
	const char* QRYFMT = "insert into %s_history select * from _%s_tail;";
	const char* errmsg;
	apr_status_t res = APR_SUCCESS;

	TR1(("---- HARVEST %s STARTED ---- \n", tbl));

	/* rename the file, e.g. queries_tail.dat -> _queries_tail.dat */
	if (0 != do_rename(tbl))
	{
		TR1(("---- HARVEST %s FAILED to rename ---- \n", tbl));
		return 1;
	}

	sprintf(qrybuf, QRYFMT, tbl, tbl);
	errmsg = gpdb_exec(&conn, &result, qrybuf);
	if (errmsg)
	{
		char src[200];
		char dst[200];
		sprintf(dst, "%s%s_tail.dat", GPMON_DIR, tbl);
		sprintf(src, "%s_%s_tail.dat", GPMON_DIR, tbl);
		/* import failed so put the file back.
		 We don't need to syncronize this as the writing
		 is done by the same thread that does the harvest */
		rename(src, dst);
		res = 1;
		TR1(("---- HARVEST %s FAILED ---- \n", tbl));
	}
	else
		TR1(("---- HARVEST %s OK ---- \n", tbl));

	PQclear(result);
	PQfinish(conn);
	return res;
}

apr_status_t gpdb_harvest(void)
{
	if (harvest("system") != APR_SUCCESS || harvest("queries") != APR_SUCCESS
			|| harvest("iterators") != APR_SUCCESS || harvest("database")
			!= APR_SUCCESS)
	{
		return 1;
	}
	return APR_SUCCESS;
}
