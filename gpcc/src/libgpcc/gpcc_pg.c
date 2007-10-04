#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <string.h>
#include <gpcc.h>
/*#include <gpcc_config.h>*/
#include <gpcc_proc.h>
#include <gpcc_file.h>
#include <gpcc_pg.h>

#include <apr_strings.h>

#define DEFAULT_GP_DBNAME         "template1"



APR_DECLARE(apr_status_t) gpcc_pg_ctl_status(int port, const char *data_dir, apr_pool_t *p) 
{
    apr_proc_t proc;
    int e = 0;
    char line[1024];

    e = gpcc_proc_pg_ctl_status(&proc, port, data_dir, 1, p);
    if (e) {
	gpcc_log_error("pg_ctl status failed");
	return e;
    }

    /* get the first line */
    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	gpcc_log_error("get string from pg_ctl status failure\n");
	return e;
    }
    
    if (strstr(line, "server is running") == 0) {
	gpcc_log_error("pg_ctl status (%s)", line);
	return 1;
    }
    gpcc_log_info("gp_ctl status (%s)", line);
    return 0;
}

APR_DECLARE(apr_status_t) gpcc_pg_hba_append(const char *data_dir, const char *const *hosts, apr_pool_t *p) {
    int i;
    FILE *fp = 0;
    char fname[512];
    
    sprintf(fname, "%s/pg_hba.conf", data_dir);
    if ((fp = fopen(fname, "ab")) == 0) {
	gpcc_log_error("open file %s failure\n", fname);
	return 1;
    }
    
    fprintf(fp, "# Greenplum Database Host Configuration\n");
    for (i = 0 ; hosts[i] ; i++) {
	fprintf(fp, "host all all %s/32 trust\n", hosts[i]);
    }
    

    if (fp)
	fclose(fp);
	
    return 0;
}

APR_DECLARE(apr_status_t) gpcc_pg_log_rotate(const char *data_dir, apr_pool_t *p) 
{
    apr_time_t now = apr_time_now();
    apr_time_exp_t exp;
    char logf[512];
    char clog[512];

    sprintf(clog, "%s.log", data_dir);
    apr_time_exp_lt(&exp, now);
    sprintf(logf, "%s.log_%d%d%d", data_dir, exp.tm_year + 1900, exp.tm_mon + 1, exp.tm_mday);

    printf(logf);

    if (gpcc_file_exists(clog)) {
	if (gpcc_file_exists(logf)) {
	    /* append the current log to backup log */
	    apr_file_append(clog, logf, 0600, p);
	    apr_file_remove(clog, p);
	} else {
	    /* rename the current log to backup log */
	    apr_file_rename(clog, logf, p);
	}
    }

    return 0;
}


APR_DECLARE(apr_status_t) gpcc_pg_pid_active(int port, int *pid, apr_pool_t *p)
{
    char lockf[512];
    apr_proc_t proc;
    char cmd[1024];
    char *argv[2];
    int e;
    int lockf_exist = 0;

    sprintf(lockf, "/tmp/.s.PGSQL.%d.lock", port);
    sprintf(cmd, "export PATH=\"/usr/sbin:/usr/bin\" && netstat -an 2> /dev/null | grep \".s.PGSQL.%d\" | awk '{print $NF}' | head -1", port);
    argv[0] = cmd;
    argv[1] = 0;

    lockf_exist = gpcc_file_exists(lockf);
    e = gpcc_proc_run_shell(&proc, argv[0], (const char * const *) argv, 0, 1, p);
    if (e) {
	gpcc_log_error("netstat failure");
	return 1;
    }

    if ((e = apr_file_gets(cmd, sizeof(cmd), proc.out)) != 0) {
	gpcc_log_info("No postgres process running with port = %d", port);
	*cmd = 0;
    }

    if (*cmd == 0 && lockf_exist == 0) {
	*pid = 0;
	gpcc_log_info("no socket connection or lock file in /tmp found for postgres");
	return 3;
    }
    else {
	FILE *fp = 0;
	char line[1024];

	if (lockf_exist == 1) {
	    if (*cmd == 0) {
		gpcc_warning(FLINE, "Have lock file %s but no process running", lockf);
	    }
	
	    if ((fp = fopen(lockf, "rb")) == 0) {
		gpcc_log_error("open file %s error", lockf);
		return 4;
	    }

	    if (fgets(line, sizeof(line), fp) == 0)  {
		gpcc_log_error("read file %s error", lockf);
		return 5;
	    }

	    *pid = strtol(line, 0, 0);

	    if (fp) fclose(fp);
	    
	} else {
	    *pid = 1;
	    gpcc_warning(FLINE, "No lock file %s but process running", lockf);
	}

	return 0;
    }

    return 0;
}


APR_DECLARE(apr_status_t) gpcc_pg_reset_invalid_segment(const char *hostname, int master_port, apr_pool_t *p) 
{
    apr_proc_t proc;
    int e, nfail;
    char line[1024];

    gpcc_log_info("gpcc_pg_reset_invalid_segment.");
    if ((e = gpcc_proc_psql(&proc, hostname, master_port, DEFAULT_GP_DBNAME, "show gp_fault_action", 1, 1, p)) != 0) {
	gpcc_log_error("psql -c 'show gp_fault_action' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }

    if (strcmp(line, "continue") != 0) {
	return 0;
    }
    

    if ((e = gpcc_proc_psql(&proc, hostname, master_port, DEFAULT_GP_DBNAME, "select count(*) from gp_configuration where valid='f' and content <> -1", 1, 1, p)) != 0) {
	gpcc_log_error("psql -c 'select count(*) from gp_configuration...' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }    
    
    nfail = strtol(line, 0, 0);

    if (nfail > 0) {

	if ((e = gpcc_proc_psql(&proc, hostname, master_port, DEFAULT_GP_DBNAME, "update gp_configuration set valid='t' where valid='f' and content <> -1", 1, 1, p)) != 0) {
	    gpcc_log_error("psql -c 'update gp_configuration...(1)' error");
	    return e;
	}

	if ((e = gpcc_proc_psql(&proc, hostname, master_port, DEFAULT_GP_DBNAME, "update gp_configuration set isprimary='t' where definedprimary='t' and isprimary='f' and content<>-1", 1, 1, p)) != 0) {
	    gpcc_log_error("psql -c 'update gp_configuration...(2)' error");
	    return e;
	}

	if ((e = gpcc_proc_psql(&proc, hostname, master_port, DEFAULT_GP_DBNAME, "update gp_configuration set isprimary='f' where definedprimary='f' and isprimary='t' and content<>-1", 1, 1, p)) != 0) {
	    gpcc_log_error("psql -c 'update gp_configuration...(3)' error");
	    return e;
	}

    }
	
	
    return 0;
}



APR_DECLARE(apr_status_t) pg_check_segment(const char *hostname, int port, apr_pool_t *p) 
{
    apr_proc_t proc;
    int e;
    char line[1024];

    e = gpcc_proc_psql(&proc, hostname, port, DEFAULT_GP_DBNAME, "show lc_collate", 1, 1, p);
    if (e) {
	gpcc_log_error("psql -c 'show lc_collate' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }

    gpcc_log_info("show lc_collate = %s\n", line);

    e = gpcc_proc_psql(&proc, hostname, port, DEFAULT_GP_DBNAME, "show lc_monetary", 1, 1, p);
    if (e) {
	gpcc_log_error("psql -c 'show lc_monetart' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }

    gpcc_log_info("show lc_monetary = %s\n", line);

    e = gpcc_proc_psql(&proc, hostname, port, DEFAULT_GP_DBNAME, "show lc_numeric", 1, 1, p);
    if (e) {
	gpcc_log_error("psql -c 'show lc_numeric' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }

    gpcc_log_info("show lc_numeric = %s\n", line);


    /* TODO: check the values with the master */

    return 0;
}

APR_DECLARE(apr_status_t) gpcc_pg_get_qe_details(const char *hostname, int port, apr_pool_t *p) 
{
    apr_proc_t proc;
    char *sql;
    char line[1024];
    int e;

    sql = "select a.hostname, a.datadir, a.port, b.valid, b.definedprimary from gp_configuration a, gp_pgdatabase where a.dbid=b.dbid and a.content<>-1";
    e = gpcc_proc_psql(&proc, hostname, port, DEFAULT_GP_DBNAME, sql, 1, 1, p);
    if (e) {
	gpcc_log_error("psql -c 'select a.hostname....' error");
	return e;
    }

    while (0 != (e = apr_file_gets(line, sizeof(line), proc.out))) {
	;
    }

    return 0;
}

APR_DECLARE(apr_status_t) gpcc_pg_change_password(const char *hostname, int port, const char *user, const char *password, apr_pool_t *p) 
{
    int e;
    apr_proc_t proc;
    char sql[1024];

    sprintf(sql, "alter user %s password '%s'", user, password);
    e = gpcc_proc_psql(&proc, hostname, port, DEFAULT_GP_DBNAME, sql, 1, 1, p); 
    if (e) {
	gpcc_log_error("gpcc_pg_change_password failure");
	return e;
    }

    return 0;
}


APR_DECLARE(apr_status_t) gpcc_pg_create_database(const char *hostname, int port, const char *dbname, apr_pool_t *p) 
{
    int e;
    apr_proc_t proc;
    char sql[1024];

    sprintf(sql, "create database %s", dbname);
    e = gpcc_proc_psql(&proc, hostname, port, DEFAULT_GP_DBNAME, sql, 1, 1, p); 
    if (e) {
	gpcc_log_error("gpcc_pg_change_password failure");
	return e;
    }

    return 0;
    


}

APR_DECLARE(apr_status_t) gpcc_pg_update_gp_configuration(const char *dbhost, 
							  int port, 
							  int dbid,
							  int content,
							  const char *primary,
							  const char *hostname,
							  int seg_port,
							  const char *data_dir,
							  apr_pool_t *p)
{
    int e;
    apr_proc_t proc;
    char sql[1024];
    char line[1024];
    int count = 0;

    /* check gp_configuration first */
    sprintf(sql, "select count(*) from gp_configuration where content=%d and definedprimary='%s'",
	    content, primary);
    e = gpcc_proc_psql(&proc, dbhost, port, DEFAULT_GP_DBNAME, sql, 1, 1, p);
    if (e) {
	gpcc_log_error("gpcc_pg_update_gp_configuration select failure");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }
    
    count = strtol(line, 0, 0);
    if (count > 0) {
	gpcc_warning(FLINE, "Content %d already exists in gp_configuration", content);
	return 0;

    }

    sprintf(sql, "insert into gp_configuration (dbid, isprimary, content, valid, definedprimary, hostname, port, datadir) values (%d, '%s', %d, 't', '%s', '%s', %d, '%s')",
	    dbid, primary, content, primary, hostname, seg_port, data_dir);
    e = gpcc_proc_psql(&proc, dbhost, port, DEFAULT_GP_DBNAME, sql, 1, 1, p);
    if (e) {
	gpcc_log_error("gpcc_pg_update_gp_configuration update failure");
	return e;
    }
    return 0;
}


APR_DECLARE(apr_status_t) gpcc_pg_update_gp_id(const char *hostname,
					       int port, 
					       const char *gpname,
					       int numseg,
					       int dbid,
					       int content,
					       apr_pool_t *p)
{
    int e;
    apr_proc_t proc;
    char sql[1024];

    sprintf(sql, "insert into gp_id (gpname, numsegments, dbid, content) values ('%s', %d, %d, %d)",
	    gpname, numseg, dbid, content);
    e = gpcc_proc_psql(&proc, hostname, port, DEFAULT_GP_DBNAME, sql, 1, 1, p);
    if (e) {
	gpcc_log_error("gpcc_pg_update_gp_id update failure");
	return e;
    }
    return 0;
}


/**
 * if QD 
 *   #port     port = 5432
 *   #log_statement log_statement=all
 *   checkpoint_segments checkpoint_segments=31
 *   stats_command_string stats_command_string=on 
 * else QE 
 *   #port 
 *   listen_addresses -- listen_addresses='*'
 *   checkpoint_segments checkpoint_segments=31
 * endif
 */
APR_DECLARE(apr_status_t) gpcc_pg_update_postgresql_conf(const char *data_dir, gpcc_pg_conf_subst_t *subst, apr_pool_t *p) {
    char *fpath;
    char *buf;
    int len = 0;
    char *tok, *last;
    FILE *fp;
    int i, e;

#define TMP_POSTGRESQL_CONF "/tmp/postgresql.conf"

    apr_filepath_merge(&fpath, data_dir, "postgresql.conf", 0, p);
    len = gpcc_file_read_fully(fpath, &buf, p);
    if (len <= 0) {
	gpcc_warning(FLINE, "open %s error", fpath);
	return 1;
    }

    if ((fp = fopen(TMP_POSTGRESQL_CONF, "wb")) == 0){
	gpcc_warning(FLINE, "open %s error", TMP_POSTGRESQL_CONF);
	return 1;
    }

    for (i = 0 ; subst[i].pattern ; i++) {
	subst[i].inserted = 0;
    }

    for (tok = apr_strtok(buf, "\r\n", &last) ; tok ; tok = apr_strtok(0, "\r\n", &last)) {
	for (i = 0 ; subst[i].pattern ; i++) {
	    if (tok == strstr(tok, subst[i].pattern)) {
		/* insert the line and # as comment*/
		fprintf(fp, "%s #", subst[i].insert);
		subst[i].inserted = 1;
		break;
	    }
	}
	fprintf(fp, "%s\n", tok);
    }

    /* add to the end of file */
    for (i = 0 ; subst[i].pattern ; i++) {
	if (subst[i].inserted == 0) {
	    fprintf(fp, "%s\n", subst[i].insert);
	    subst[i].inserted = 1;
	}
    }


    if (fp) fclose(fp);

    if (0 != (e = apr_file_rename(TMP_POSTGRESQL_CONF, fpath, p))) {
	gpcc_warning(FLINE, "rename %s to %s error", TMP_POSTGRESQL_CONF, fpath);
	remove(TMP_POSTGRESQL_CONF);
	return e;
    }



    return 0;
}

