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
#include <limits.h>
#include <string.h>
#include <gpcc.h>
#include <gpcc_config.h>
#include <gpcc_proc.h>
#include <gpcc_pg.h>


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

static int file_exists(const char *fpath)
{
    int fd;
    if (-1 == (fd = open(fpath, O_RDONLY, 0600))) {
	gpcc_warning(FLINE, "unable to open file %s", fpath);
	return 0;
    }

    close(fd);
    return 1;
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

    if (file_exists(clog)) {
	if (file_exists(logf)) {
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

    lockf_exist = file_exists(lockf);
    e = gpcc_proc_run_shell(&proc, argv[0], (const char * const *) argv, 0, 1, p);
    if (e) {
	gpcc_log_error("netstat failure");
	return 1;
    }

    if ((e = apr_file_gets(cmd, sizeof(cmd), proc.out)) != 0) {
	gpcc_warning(FLINE, "No postgres process running with port = %d", port);
	*cmd = 0;
    }

    if (*cmd == 0 && lockf_exist == 0) {
	*pid = 0;
	gpcc_log_error("no socket connection or lock file in /tmp found for postgres");
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


APR_DECLARE(apr_status_t) gpcc_pg_reset_invalid_segment(int master_port, apr_pool_t *p) 
{
    apr_proc_t proc;
    int e, nfail;
    char line[1024];

    if ((e = gpcc_proc_psql(&proc, master_port, DEFAULT_GP_DBNAME, "show gp_fault_action", 1, 1, p)) != 0) {
	gpcc_log_error("psql -c 'show gp_fault_action' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }

    if (strcmp(line, "continue") != 0) {
	return 0;
    }
    

    if ((e = gpcc_proc_psql(&proc, master_port, DEFAULT_GP_DBNAME, "select count(*) from gp_configuration where valid='f' and content <> -1", 1, 1, p)) != 0) {
	gpcc_log_error("psql -c 'select count(*) from gp_configuration...' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }    
    
    nfail = strtol(line, 0, 0);

    if (nfail > 0) {

	if ((e = gpcc_proc_psql(&proc, master_port, DEFAULT_GP_DBNAME, "update gp_configuration set valid='t' where valid='f' and content <> -1", 1, 1, p)) != 0) {
	    gpcc_log_error("psql -c 'update gp_configuration...(1)' error");
	    return e;
	}

	if ((e = gpcc_proc_psql(&proc, master_port, DEFAULT_GP_DBNAME, "update gp_configuration set isprimary='t' where definedprimary='t' and isprimary='f' and content<>-1", 1, 1, p)) != 0) {
	    gpcc_log_error("psql -c 'update gp_configuration...(2)' error");
	    return e;
	}

	if ((e = gpcc_proc_psql(&proc, master_port, DEFAULT_GP_DBNAME, "update gp_configuration set isprimary='f' where definedprimary='f' and isprimary='t' and content<>-1", 1, 1, p)) != 0) {
	    gpcc_log_error("psql -c 'update gp_configuration...(3)' error");
	    return e;
	}

    }
	
	
    return 0;
}



APR_DECLARE(apr_status_t) pg_check_segment(int port, apr_pool_t *p) 
{
    apr_proc_t proc;
    int e;
    char line[1024];

    e = gpcc_proc_psql(&proc, port, DEFAULT_GP_DBNAME, "show lc_collate", 1, 1, p);
    if (e) {
	gpcc_log_error("psql -c 'show lc_collate' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }

    gpcc_log_info("show lc_collate = %s\n", line);

    e = gpcc_proc_psql(&proc, port, DEFAULT_GP_DBNAME, "show lc_monetary", 1, 1, p);
    if (e) {
	gpcc_log_error("psql -c 'show lc_monetart' error");
	return e;
    }

    if ((e = apr_file_gets(line, sizeof(line), proc.out)) != 0) {
	return e;
    }

    gpcc_log_info("show lc_monetary = %s\n", line);

    e = gpcc_proc_psql(&proc, port, DEFAULT_GP_DBNAME, "show lc_numeric", 1, 1, p);
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

APR_DECLARE(apr_status_t) gpcc_pg_change_password(int port, const char *user, const char *password, apr_pool_t *p) 
{
    int e;
    apr_proc_t proc;
    char sql[1024];

    sprintf(sql, "alter user %s password '%s'", user, password);
    e = gpcc_proc_psql(&proc, port, DEFAULT_GP_DBNAME, sql, 1, 1, p); 
    if (e) {
	gpcc_log_error("gpcc_pg_change_password failure");
	return e;
    }

    return 0;
}


APR_DECLARE(apr_status_t) gpcc_pg_create_database(int port, const char *dbname, apr_pool_t *p) 
{
    int e;
    apr_proc_t proc;
    char sql[1024];

    sprintf(sql, "create database %s", dbname);
    e = gpcc_proc_psql(&proc, port, DEFAULT_GP_DBNAME, sql, 1, 1, p); 
    if (e) {
	gpcc_log_error("gpcc_pg_change_password failure");
	return e;
    }

    return 0;
    


}
