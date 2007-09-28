#include <stdio.h>
#include <stdlib.h>
#include <apr_strings.h>
#include <gpcc.h>
#include <gpcc_proc.h>

static char *GPHOME = 0;
static char LD_LIBRARY_PATH[1024];
static char PATH[1024];

static int initialize = 0;

static apr_status_t gpcc_proc_initialize() 
{
    char *path = 0;

    if (initialize) 
	return 0;
    
    GPHOME = getenv("GPHOME");
    if (GPHOME == 0) {
	gpcc_fatal(FLINE, "GPHOME not set");
	return 1;
    }
    
    *LD_LIBRARY_PATH = 0;
    path = getenv("LD_LIBRARY_PATH");
    if (path) {
	strcpy(LD_LIBRARY_PATH, path);
    }

    sprintf(LD_LIBRARY_PATH, "%s:%s/lib", LD_LIBRARY_PATH, GPHOME);

    *PATH = 0;
    path = getenv("PATH");
    if (path) {
	strcpy(PATH, path);
    }
    sprintf(PATH, "%s:%s/bin", PATH, GPHOME);

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
    if ((e = apr_procattr_io_set(attr, APR_NO_PIPE, APR_FULL_BLOCK, APR_NO_PIPE)) != APR_SUCCESS) {
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
    char cmd[1024];
    int e;

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    *cmd = 0;
    if (utility)
	sprintf(cmd, "export LD_LIBRARY_PATH=%s && PGOPTIONS=\"-c gp_session_role=utility\" ", LD_LIBRARY_PATH);
    
    strcat(cmd, GPHOME);
    strcat(cmd, "/bin/psql");
    
    if (hostname) {
	strcat(cmd, " -h ");
	strcat(cmd, hostname);
    }
    
    sprintf(cmd, "%s -p %d %s -A -t -c \"%s\"", cmd, port, dbname, sql);

    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, wait, p);

    gpcc_log_info("Execute: %s [ret=%d]", cmd, e);

    return e;
}

APR_DECLARE(apr_status_t) gpcc_proc_ssh(apr_proc_t *proc, const char *user, const char *hostname, int port, const char *cmd, int wait, apr_pool_t *p)
{
    char *argv[10];
    int e;
    char host[256];
    char portstr[10];
    char cmdstr[1024];

    sprintf(portstr, "%d", port);
    sprintf(host, "%s@%s", user, hostname);
    sprintf(cmdstr, "\"\"%s\"\"", cmd);
    argv[0] = "ssh";
    argv[1] = "-p";
    argv[2] = portstr;
    argv[3] = "-o";
    argv[4] = "\"\"BatchMode yes\"\"";
    argv[5] = "-o";
    argv[6] = "\"\"StrictHostKeyChecking no\"\"";
    argv[7] = host;
    argv[8] = cmdstr;
    argv[9] = 0;

    e = gpcc_proc_run_prog(proc, "ssh", (const char *const *)argv, 0, wait, p);
    return e;
}

APR_DECLARE(apr_status_t) gpcc_proc_gpssh(apr_proc_t **proc, const char *user, const char *const *hostnames, 
					  int port, const char *cmd, int wait, apr_pool_t *pool)
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
	e = gpcc_proc_ssh(p, user, hostnames[i], port, cmd, 0, pool);
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

APR_DECLARE(apr_status_t) gpcc_proc_scp(apr_proc_t *proc, int port, const char *src, const char *target, int wait, apr_pool_t *p)
{
    char *argv[10];
    int e;
    char portstr[10];

    sprintf(portstr, "%d", port);
    argv[0] = "scp";
    argv[1] = "-pr";
    argv[2] = "-P";
    argv[3] = portstr;
    argv[4] = (char *) src;
    argv[5] = (char *) target;
    argv[6] = 0;

    e = gpcc_proc_run_prog(proc, "scp", (const char *const *)argv, 0, wait, p);
    return e;
}

/* scp -pr data_dir user@host:/dir */
APR_DECLARE(apr_status_t) gpcc_proc_gpscp(apr_proc_t **proc, const char *user, const char *const *hostnames, 
					  int port, const char *src, const char *target, int wait, apr_pool_t *pool) 
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
	char *source = apr_psprintf(pool, "%s@%s:%s", user, hostnames[i], src);
	e = gpcc_proc_scp(p, port, source, target, 0, pool);
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
    char cmd[1024];
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "export LD_LIBRARY_PATH=%s && export PATH=%s && initdb -E %s -D %s --locale=%s --max_connections=%d --shared_buffers=%s > /dev/null 2>&1", 
	    LD_LIBRARY_PATH, PATH, encoding, data_dir, locale, max_connect, shared_buffer);
    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);

    return e;
}


/**
 *  pg_ctl start [-w] [-D DATADIR] [-s] [-l FILENAME] [-o "OPTIONS"]
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start(apr_proc_t *proc, int port, const char *data_dir, const char *option, int wait, int blocking, apr_pool_t *p)
{
    int e;
    char cmd[1024];
    char *waitflag = (wait ? "-w" : "");
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "export LD_LIBRARY_PATH=%s && export PATH=%s && export PGPORT=%d && pg_ctl start -D %s -l %s.log -o \"%s\" %s > /dev/null 2>&1", 
	    LD_LIBRARY_PATH, PATH, port, data_dir, data_dir, option, waitflag);
    
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
    char cmd[1024];
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "export LD_LIBRARY_PATH=%s && export PATH=%s && gpsyncmaster -p %d -D %s -i >> %s.log 2>&1 &", 
	    LD_LIBRARY_PATH, PATH, port, data_dir, data_dir);
    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);

    return e;
}

APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start_segment(apr_proc_t *proc, int port, const char *data_dir, 
							 int utility, int wait, int blocking, apr_pool_t *p)
{
    char option[128];
    if (utility) {
	sprintf(option, "-i -p %d -c gp_role=utility", port);
    } else {
	sprintf(option, "-i -p %d", port);
    }
    return gpcc_proc_pg_ctl_start(proc, port, data_dir, option, wait, blocking, p);    
}

/**
 * pg_ctl start master in admin mode
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start_master(apr_proc_t *proc, int port, const char *data_dir, 
							int utility, int wait, int blocking, apr_pool_t *p)
{
    char option[128];
    if (utility) {
	sprintf(option, "-i -p %d -c gp_role=utility", port);
    } else {
	sprintf(option, "-i -E -p %d", port);
    }
    return gpcc_proc_pg_ctl_start(proc, port, data_dir, option, wait, blocking, p);
}


/**
 *  pg_ctl stop [-W] [-D DATADIR] [-s] [-m SHUTDOWN-MODE]
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_stop(apr_proc_t *proc, int port, const char *data_dir, const char *mode, int wait, int blocking, apr_pool_t *p) 
{
    int e;
    char cmd[1024];
    char *argv[2];
    char *waitflag = (wait ? "-w" : "-W");
    char option[128];
    sprintf(option, "-i -p %d", port);

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "export LD_LIBRARY_PATH=%s && export PATH=%s && export PGPORT=%d && pg_ctl stop -D %s -o \"%s\" -m %s %s > /dev/null 2>&1", 
	    LD_LIBRARY_PATH, PATH, port, data_dir, option, mode, waitflag);
    
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
    char cmd[1024];
    char *argv[2];

    if ((e = gpcc_proc_initialize()) != 0) 
	return e;

    sprintf(cmd, "export LD_LIBRARY_PATH=%s && export PATH=%s && export PGPORT=%d && pg_ctl status -D %s", 
	    LD_LIBRARY_PATH, PATH, port, data_dir);
    
    argv[0] = cmd;
    argv[1] = 0;
    e = gpcc_proc_run_shell(proc, argv[0], (const char *const *) argv, 0, blocking, p);    

    return e;

}
