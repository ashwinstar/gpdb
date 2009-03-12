#include <stdio.h>
#include <stdlib.h>
#include <apr_strings.h>
#include <gpcc.h>
#include <gpcc_strings.h>
#include <gpcc_proc.h>

static char *GPHOME = 0;
static char LD_LIBRARY_PATH[1024];
static char PATH[1024];

static int initialize = 0;

static apr_status_t gpcc_proc_initialize() 
{
    char *path = 0;
    int len;
    int i;
    char *cmdpath[10] = {"/usr/kerberos/bin", "/usr/sfw/bin", "/opt/sfw/bin", "/usr/local/bin", 
			 "/bin", "/usr/bin", "/sbin", "/usr/sbin", "/usr/ucb", "/sw/bin" };

    if (initialize) 
	return 0;
    
    GPHOME = getenv("GPHOME");
    if (GPHOME == 0) {
	gpcc_fatal(FLINE, "GPHOME not set");
	return 1;
    }
    
    GPHOME = gpcc_string_trim(GPHOME);
    len = strlen(GPHOME);
    if (GPHOME[len-1] == '/')
       GPHOME[len-1] = 0;

    *LD_LIBRARY_PATH = 0;
    path = getenv("LD_LIBRARY_PATH");
    if (path) {
	strcpy(LD_LIBRARY_PATH, path);
    }

    sprintf(LD_LIBRARY_PATH, "%s/lib", GPHOME);

    *PATH = 0;
    sprintf(PATH, "%s/bin:.", GPHOME);
    for (i = 0 ; i < gpcc_lengthof(cmdpath) ; i++) {
	strcat(PATH, cmdpath[i]);
	strcat(PATH, ":");
    }

    initialize = 1;
    return 0;
}



APR_DECLARE(apr_status_t) gpcc_proc_run(apr_proc_t *proc, const char *prog, const char *const *argv, const char * const *env, apr_cmdtype_e cmdtype, int wait, apr_pool_t *p)
{
    int e;
    int ret = 0;
    apr_procattr_t *attr;
    apr_exit_why_e exitwhy;

    e = apr_procattr_create(&attr, p);
    if (e) {
	gpcc_log_error("apr_procattr_create failure");
	return e;
    }
    if ((e = apr_procattr_io_set(attr, APR_FULL_BLOCK, APR_FULL_BLOCK, APR_NO_PIPE)) != APR_SUCCESS) {
	return e;
    }
    /* default value: APR_PROGRAM */
    if ((e = apr_procattr_cmdtype_set(attr, cmdtype)) != APR_SUCCESS) {
        return e;
    }

    e = apr_proc_create(proc, prog, argv, env, attr, p);
    if (e){
	gpcc_log_error("apr_proc_create failure");
	return e;
    }
    

    if (wait) {
	e = apr_proc_wait(proc, &ret, &exitwhy, APR_WAIT);
	return ret;
    }

    return e;

}


APR_DECLARE(apr_status_t) gpcc_proc_run_shell(apr_proc_t *proc, const char *prog, const char *const *argv, const char * const *env, int wait, apr_pool_t *p)
{
    apr_cmdtype_e cmdtype = APR_SHELLCMD;

    if (env) {
	cmdtype = APR_SHELLCMD_ENV;
    }

    gpcc_log_info("Execute '%s'", prog);
    return gpcc_proc_run(proc, prog, argv, env, cmdtype, wait, p);

}

APR_DECLARE(apr_status_t) gpcc_proc_run_prog(apr_proc_t *proc, const char *prog, const char *const *argv, const char *const *env, int wait, apr_pool_t *p)
{
    apr_cmdtype_e cmdtype = APR_PROGRAM_PATH;
    if (env) {
	cmdtype = APR_PROGRAM_ENV;
    }
    return gpcc_proc_run(proc, prog, argv, env, cmdtype, wait, p);
}

APR_DECLARE(apr_status_t) gpcc_proc_psql(apr_proc_t *proc, const char *hostname, int port, const char *dbname, const char *sql, 
					 int utility, int wait, apr_pool_t *p)
{
    char *argv[2];
    char cmd[2048];
    int e;

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    *cmd = 0;
    if (utility)
	sprintf(cmd, "LD_LIBRARY_PATH=%s && export LD_LIBRARY_PATH && PGOPTIONS=\"-c gp_session_role=utility\" ", LD_LIBRARY_PATH);
    
    strcat(cmd, GPHOME);
    strcat(cmd, "/bin/psql");
    
    if (hostname) {
	strcat(cmd, " -h ");
	strcat(cmd, hostname);
    }
    
    sprintf(cmd, "%s -p %d -d %s -A -t -c \"%s\"", cmd, port, dbname, sql);

    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, wait, p);

    gpcc_log_info("Execute: %s [ret=%d]", cmd, e);

    return e;
}

APR_DECLARE(apr_status_t) gpcc_proc_ssh(apr_proc_t *proc, const char *hostname, const char *cmd, int wait, apr_pool_t *p)
{
    char *argv[10];
    int e;
    char cmdstr[2048];

    sprintf(cmdstr, "ssh -o \"BatchMode yes\" -o \"StrictHostKeyChecking no\" %s '%s'",
	    hostname, cmd);

    argv[0] = cmdstr;
    argv[1] = 0;

    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, wait, p);
    
    return e;
}

APR_DECLARE(apr_status_t) gpcc_proc_gpssh(apr_proc_t **proc, const char *const *hostnames, 
					  const char *cmd, int wait, apr_pool_t *pool)
{
    int i, nhosts, n;
    int e = 0;
    int ret;
    apr_proc_t *p;
    apr_exit_why_e exitwhy;
    
    for (i = 0 ; hostnames[i] ; i++) { ; }
    nhosts = i;

    *proc = apr_palloc(pool, sizeof(apr_proc_t) * nhosts);    
    n = 0;
    for (i = 0, p = *proc ; hostnames[i] ; i++, p++) {
	e = gpcc_proc_ssh(p, hostnames[i], cmd, 0, pool);
	if (e) {
	    gpcc_log_error("gpcc_proc_ssh %s: command failed %s\n", hostnames[i], cmd);
	    return e;
	}
    }
 
    if (wait) {
	e = 0;
	for (i = 0, p = *proc ; hostnames[i] ; i++, p++) {
	    apr_proc_wait(p, &ret, &exitwhy, APR_WAIT);
	    e |= ret;
	}

	return e;
    }  

    return e;
}

APR_DECLARE(apr_status_t) gpcc_proc_scp(apr_proc_t *proc, const char *src, const char *target, int wait, apr_pool_t *p)
{
    char *argv[10];
    int e;

    argv[0] = "scp";
    argv[1] = "-pr";
    argv[2] = (char *) src;
    argv[3] = (char *) target;
    argv[4] = 0;

    e = gpcc_proc_run_prog(proc, "scp", (const char *const *)argv, 0, wait, p);
    return e;
}

/* scp -pr data_dir user@host:/dir */
APR_DECLARE(apr_status_t) gpcc_proc_gpscp(apr_proc_t **proc, const char *const *hostnames, 
					  const char *src, const char *target, int wait, apr_pool_t *pool) 
{
    int i, nhosts;
    int e = 0;
    int ret;
    apr_proc_t *p;
    apr_exit_why_e exitwhy;
    
    for (i = 0 ; hostnames[i] ; i++) { ; }
    nhosts = i;

    *proc = apr_palloc(pool, sizeof(apr_proc_t) * nhosts);

    for (i = 0, p = *proc ; hostnames[i] ; i++, p++) {
	char *source = apr_psprintf(pool, "%s:%s", hostnames[i], src);
	e = gpcc_proc_scp(p, source, target, 0, pool);
	if (e) {
	    gpcc_log_error("gpcc_proc_scp %s: command failed copy %s to %s\n", hostnames[i], src, target);
	    return e;
	}
	
    }
 
    if (wait) {
	e = 0;
	for (i = 0 , p = *proc ; hostnames[i] ; i++, p++) {
	    apr_proc_wait(p, &ret, &exitwhy, APR_WAIT);
	    e |= ret;
	}

	return e;
    }  

    return e;
}

/**
 *  initdb -E encoding -D data_dir --locale=locale --max_connections=25 --shared_buffers=12000Mb
 */
APR_DECLARE(apr_status_t) gpcc_proc_initdb(apr_proc_t *proc, const char *data_dir, const char *encoding, const char *locale, 
					   int max_connect, const char *shared_buffer, int blocking, apr_pool_t *p)
{
    int e;
    char cmd[2048];
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "LD_LIBRARY_PATH=%s && PATH=%s && export LD_LIBRARY_PATH PATH && %s/bin/initdb -E %s -D %s --locale=%s --max_connections=%d --shared_buffers=%s > /dev/null 2>&1", 
	    LD_LIBRARY_PATH, PATH, GPHOME, encoding, data_dir, locale, max_connect, shared_buffer);
    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);

    return e;
}


/**
 *  pg_ctl start [-w] [-D DATADIR] [-s] [-l FILENAME] [-o "OPTIONS"]
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start(apr_proc_t *proc, const char *host, int port, const char *data_dir, const char *option, int wait, int blocking, apr_pool_t *p)
{
    int e;
    char cmd[2048];
    char *waitflag = (wait ? "-w" : "");
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    if (!host) {
	sprintf(cmd, "LD_LIBRARY_PATH=%s && PATH=%s && PGPORT=%d && export LD_LIBRARY_PATH PATH PGPORT && %s/bin/pg_ctl start -D %s -l %s.log -o \"%s\" %s >> %s.log 2>&1", 
		LD_LIBRARY_PATH, PATH, port, GPHOME, data_dir, data_dir, option, waitflag, data_dir);
    } else {
	sprintf(cmd, "ssh %s 'LD_LIBRARY_PATH=%s && PATH=%s && PGPORT=%d && export LD_LIBRARY_PATH PATH PGPORT && %s/bin/pg_ctl start -D %s -l %s.log -o \"%s\" %s >> %s.log 2>&1'", 
		host, LD_LIBRARY_PATH, PATH, port, GPHOME, data_dir, data_dir, option, waitflag, data_dir);	
    }
    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);

    return e;
}

/**
 *  gpsyncmaster [-p port] [-D DATADIR] -i
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_gpsyncmaster_start(apr_proc_t *proc, int port, const char *data_dir, const char *option, int blocking, apr_pool_t *p)
{
    int e;
    char cmd[2048];
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "LD_LIBRARY_PATH=%s && PATH=%s && export LD_LIBRARY_PATH PATH && %s/bin/gpsyncmaster -p %d -D %s -i >> %s.log 2>&1 &", 
	    LD_LIBRARY_PATH, PATH, GPHOME, port, data_dir, data_dir);
    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);

    return e;
}

APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start_segment(apr_proc_t *proc, const char *host, int port, const char *data_dir, 
							 int utility, int wait, int blocking, apr_pool_t *p)
{
    char option[128];
    if (utility) {
	sprintf(option, "-i -p %d -c gp_role=utility", port);
    } else {
	sprintf(option, "-i -p %d", port);
    }
    return gpcc_proc_pg_ctl_start(proc, host, port, data_dir, option, wait, blocking, p);    
}

/**
 * pg_ctl start master in admin mode
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start_master(apr_proc_t *proc, const char *host, int port, const char *data_dir, 
							int utility, int restricted_conn, int nostandbyqd, 
							int wait, int blocking, apr_pool_t *p)
{
    char option[128];
    *option = 0;
    if (restricted_conn > 0) {
	sprintf(option, "-c superuser_reserved_connections=%d ", restricted_conn);
    }
    if (nostandbyqd) {
	strcat(option, "-y ");
    }
    if (utility) {
	sprintf(option, "%s -i -p %d -c gp_role=utility", option, port);
    } else {
	sprintf(option, "%s -i -E -p %d", option, port);
    }
    return gpcc_proc_pg_ctl_start(proc, host, port, data_dir, option, wait, blocking, p);
}


/**
 *  pg_ctl stop [-W] [-D DATADIR] [-s] [-m SHUTDOWN-MODE]
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_stop(apr_proc_t *proc, const char *host, int port, const char *data_dir, const char *mode, int wait, int blocking, apr_pool_t *p) 
{
    int e;
    char cmd[2048];
    char *argv[2];
    char *waitflag = (wait ? "-w" : "-W");
    char option[128];
    sprintf(option, "-i -p %d", port);

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    if (!host) {
	sprintf(cmd, "LD_LIBRARY_PATH=%s && PATH=%s && PGPORT=%d && export LD_LIBRARY_PATH PATH PGPORT && %s/bin/pg_ctl stop -D %s -o \"%s\" -m %s %s >> %s.log 2>&1", 
		LD_LIBRARY_PATH, PATH, port, GPHOME, data_dir, option, mode, waitflag, data_dir);
    } else {
	sprintf(cmd, "ssh %s 'LD_LIBRARY_PATH=%s && PATH=%s && PGPORT=%d && export LD_LIBRARY_PATH PATH PGPORT && %s/bin/pg_ctl stop -D %s -o \"%s\" -m %s %s >> %s.log 2>&1'", 
		host, LD_LIBRARY_PATH, PATH, port, GPHOME, data_dir, option, mode, waitflag, data_dir);
    }    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);    

    return e;
}

/**
 *  pg_ctl status [-D DATADIR]
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_status(apr_proc_t *proc, int port, const char *data_dir, int blocking, apr_pool_t *p)
{

    int e;
    char cmd[2048];
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "LD_LIBRARY_PATH=%s && PATH=%s && PGPORT=%d && export LD_LIBRARY_PATH PATH PGPORT && %s/bin/pg_ctl status -D %s", 
	    LD_LIBRARY_PATH, PATH, port, GPHOME, data_dir);
    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);    

    return e;

}

APR_DECLARE(apr_status_t) gpcc_proc_findCmdInPath(const char *cmd, char *fpath, int *len) 
{
    char *cmdpath[10] = {"/usr/kerberos/bin", "/usr/sfw/bin", "/opt/sfw/bin", "/usr/local/bin", 
			 "/bin", "/usr/bin", "/sbin", "/usr/sbin", "/usr/ucb", "/sw/bin" };
    char path[1024];
    int i;
    FILE *f;

    for (i = 0 ; i < gpcc_lengthof(cmdpath) ; i++) {
	sprintf(path, "%s/%s", cmdpath[i], cmd);
	f = fopen(path, "r");
	if (f) {
	    fclose(f);
	    if (strlen(path) > *len)
		return APR_EINVAL;
	    *len = strlen(path);
	    strcpy(fpath, path);
	    return 0;
	}
    }
    
    return APR_EINVAL;
}
