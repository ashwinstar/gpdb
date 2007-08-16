#ifndef GPCC_PROC_H
#define GPCC_PROC_H

#include <apr_thread_proc.h>
#include <apr_pools.h>

/**
 *  gpcc_proc_run_shell to run a shell command 
 *  @param proc process handle
 *  @param prog command line to run
 *  @param argv argument list (terminated with NULL element)
 *  @param env  environment list (terminated with NULL element)
 *  @param wait flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_run_shell(apr_proc_t *proc, const char *prog, const char *const *argv, const char * const *env, int wait, apr_pool_t *p);

/**
 *  gpcc_proc_run_prog to run an executable 
 *  @param proc process handle
 *  @param prog command line to run
 *  @param argv argument list (terminated with NULL element)
 *  @param env  environment list (terminated with NULL element)
 *  @param wait flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_run_prog(apr_proc_t *proc, const char *prog, const char *const *argv, const char *const *env, int wait, apr_pool_t *p);

/**
 *  gpcc_proc_psql to run a postgres SQL statement 
 *  @param proc process handle
 *  @param sql SQL statement
 *  @param wait flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_psql(apr_proc_t *proc, const char *sql, int wait, apr_pool_t *p);

/**
 *  gpcc_proc_ssh to run a ssh remote command
 *  @param proc process handle
 *  @param cmd command line to run remotely
 *  @param wait flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_ssh(apr_proc_t *proc, const char *user, const char *hostname, int port, const char *cmd, int wait, apr_pool_t *p);

/**
 *  gpssh to run command line with a list of hosts
 *  @param proc list of proc
 *  @param hostnames list of hostnames
 *  @param port ssh port number
 *  @param cmd command line to run
 *  @param wait flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_gpssh(apr_proc_t *proc, const char *user, const char *const *hostnames, int port, const char *cmd, int wait, apr_pool_t *p);
#endif
