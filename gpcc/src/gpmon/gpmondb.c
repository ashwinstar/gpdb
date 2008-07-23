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
    PGconn *conn      = NULL;
    PGresult* res = 0;
    ExecStatusType status;

    conn = PQconnectdb(connstr);
    if (PQstatus(conn) != CONNECTION_OK) {
	gpmon_fatal(FLINE, 
		    "GPDB connect error: %s\n", PQerrorMessage(conn));
    }

    TR1(("Query: %s\n", query));

    *pconn = conn;
    res = PQexec(conn, query);
    status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
	return PQerrorMessage(conn);
    }

    *pres = res;
    return 0;
}


int gpdb_get_gpmon_port(void)
{
    PGconn* conn = 0;
    PGresult* result = 0;
    char* curport = 0;
    int rowcount;
    const char* QUERY = "SELECT setting FROM pg_settings WHERE name = 'gpmon_port'";
    int port = 0;
    const char* errmsg = gpdb_exec(&conn, &result, QUERY);
    if (errmsg) {
	gpmon_fatal(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
    }

    rowcount = PQntuples(result);
    if (rowcount > 0) {
	curport = PQgetvalue(result, 0, 0);
	if (curport) {
	    port = atoi(curport);
	}
    }
    PQclear(result);
    PQfinish(conn);

    if (!port) {
	gpmon_fatal(FLINE, "Unable to retrieve gpmon_port GUC from GPDB\n");
    }
    return port;
}



void gpdb_get_hostlist(int* hostcnt, char*** hostvec, apr_pool_t* pool)
{
    PGconn* conn = 0;
    PGresult* result = 0;
    int rowcount, i;
    const char* QUERY = "SELECT DISTINCT(hostname) FROM gp_configuration WHERE valid = 't'";
    int    cnt = 0;
    char** vec = 0;
    const char* errmsg = gpdb_exec(&conn, &result, QUERY);
    if (errmsg) {
	gpmon_fatal(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
    }

    rowcount = PQntuples(result);
    if (rowcount > 0) {
	vec = apr_pcalloc(pool, sizeof(char*) * (1 + rowcount));
	CHECKMEM(vec);
	
	for (i = 0; i < rowcount; i++) {
	    char* p = PQgetvalue(result, i, 0);
	    if (!p) 
		continue;
	    p = apr_pstrdup(pool, p);
	    CHECKMEM(p);
	    p = gpmon_trim(p);
	    vec[cnt++] = p;
	}
    }
    PQclear(result);
    PQfinish(conn);

    *hostcnt = cnt;
    *hostvec = vec;
}



void gpdb_get_config_md5(unsigned char md5[APR_MD5_DIGESTSIZE])
{
    PGconn* conn = 0;
    PGresult* result = 0;
    const char* QUERY = "select content || case when definedprimary then 1 else 0 end || dbid || case when isprimary then 1 else 0 end || case when valid then 1 else 0 end || hostname || port || datadir from gp_configuration";

    apr_md5_ctx_t ctx;
    int e, i;
    int rowcount;
    const char* errmsg = gpdb_exec(&conn, &result, QUERY);
    if (errmsg) {
	gpmon_fatal(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
    }
    
    if (0 != (e = apr_md5_init(&ctx))) {
	gpmon_fatalx(FLINE, e, "apr_md5_init failed");
    }
    
    rowcount = PQntuples(result);
    for (i = 0; i < rowcount; i++) {
	const char* p = PQgetvalue(result, i, 0);
	if (p) {
	    if (0 != (e = apr_md5_update(&ctx, p, strlen(p))))
		gpmon_fatalx(FLINE, e, "apr_md5_update failed");
	}
    }
    apr_md5_final(md5, &ctx);
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
    if (errmsg) {
	gpmon_fatal(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, QUERY);
    }

    rowcount = PQntuples(result);
    if (rowcount > 0) {
	hname = PQgetvalue(result, 0, 0);
	dir = PQgetvalue(result, 0, 1);
    }

    if (!hname || !dir) {
	gpmon_fatal(FLINE, "unable to get master data directory");
    }

    hname = apr_pstrdup(pool, gpmon_trim(hname));
    CHECKMEM(hname);

    dir = apr_pstrdup(pool, gpmon_trim(dir));
    CHECKMEM(dir);

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
    if (fp) fclose(fp);


    return 0;
}

static void harvest(const char* tbl)
{
    PGconn* conn = 0;
    PGresult* result = 0;
    char qrybuf[1024*2];
    char condbuf[100];
    const char* QRYFMT = "insert into %s_history_%04d%02d select * from _%s_tail where %s;";
    const char* errmsg;
    apr_time_t now = apr_time_now();
    apr_time_t past;
    apr_time_exp_t tm;
    int e;

    TR1(("---- HARVEST %s STARTED ---- \n", tbl));
    

    /* rename the file, e.g. queries_tail.dat -> _queries_tail.dat */
    if (0 != do_rename(tbl))  {
	TR1(("---- HARVEST %s FAILED to rename ---- \n", tbl));
	return;
    }

    if (0 != (e = apr_time_exp_lt(&tm, now))) {
	gpmon_fatalx(FLINE, e, "apr_time_exp_lt failed");
    }

    /* this query is usually successful, unless we violate the partition constraint of the 
       table we are inserting into (which could happen once a month when we cross month-line */
    sprintf(qrybuf, QRYFMT, tbl, 1900 + tm.tm_year, tm.tm_mon + 1, tbl, "1=1");
    errmsg = gpdb_exec(&conn, &result, qrybuf);
    if (!errmsg) {
	TR1(("---- HARVEST %s OK ---- \n", tbl));
	PQclear(result);
	PQfinish(conn);
	return;			/* usual case */
    }

    PQclear(result);
    PQfinish(conn);

    /* if we are here, let's try to do the insert by filtering on yesterday and today dates */
    TR1(("---- HARVEST %s insert yesterday and today ---\n", tbl));

    /* insert yesterday */
    past = now - apr_time_from_sec(60 * 60 * 24);
    if (0 != (e = apr_time_exp_lt(&tm, past))) {
	gpmon_fatalx(FLINE, e, "apr_time_exp_lt failed");
    }
    sprintf(condbuf, "DATE(ctime) = DATE '%04d-%02d-%02d'", 
	    1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday);
    sprintf(qrybuf, QRYFMT, tbl, 
	    1900 + tm.tm_year, tm.tm_mon + 1, 
	    tbl, condbuf);
    /* for yesterday, we can ignore error */
    errmsg = gpdb_exec(&conn, &result, qrybuf);
    PQclear(result);
    PQfinish(conn);

    /* insert today */
    if (0 != (e = apr_time_exp_lt(&tm, now))) {
	gpmon_fatalx(FLINE, e, "apr_time_exp_lt failed");
    }
    sprintf(condbuf, "DATE(ctime) = DATE '%04d-%02d-%02d'", 
	    1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday);
    sprintf(qrybuf, QRYFMT, tbl, 
	    1900 + tm.tm_year, tm.tm_mon + 1, 
	    tbl, condbuf);
    errmsg = gpdb_exec(&conn, &result, qrybuf);
    if (errmsg) {
	gpmon_fatal(FLINE, "GPDB error %s\n\tquery: %s\n", errmsg, qrybuf);
    }
    PQclear(result);
    PQfinish(conn);
}


void gpdb_harvest(void)
{
    harvest("metrics");
    harvest("queries");
    harvest("iterators");
}
