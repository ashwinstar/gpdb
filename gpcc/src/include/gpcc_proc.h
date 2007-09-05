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
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_run_shell(apr_proc_t *proc, const char *prog, const char *const *argv, const char * const *env, int blocking, apr_pool_t *p);

/**
 *  gpcc_proc_run_prog to run an executable 
 *  @param proc process handle
 *  @param prog command line to run
 *  @param argv argument list (terminated with NULL element)
 *  @param env  environment list (terminated with NULL element)
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_run_prog(apr_proc_t *proc, const char *prog, const char *const *argv, const char *const *env, int blocking, apr_pool_t *p);

/**
 *  gpcc_proc_psql to run a postgres SQL statement 
 *  @param proc process handle
 *  @param hostname host to connect gpdb.  NULL means "localhost"
 *  @param port gpdb port number
 *  @param dbname database name
 *  @param sql SQL statement
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_psql(apr_proc_t *proc, const char *hostname, int port, const char *dbname, const char *sql, int utility, int blocking, apr_pool_t *p);

/**
 *  gpcc_proc_ssh to run a ssh remote command
 *  @param proc process handle
 *  @param cmd command line to run remotely
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param p memory pool
 *  @return APR_SUCCESS if succes otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_ssh(apr_proc_t *proc, const char *user, const char *hostname, int port, const char *cmd, int blocking, apr_pool_t *p);

/**
 *  gpssh to run command line with a list of hosts
 *  @param proc list of proc
 *  @param hostnames list of hostnames
 *  @param port ssh port number
 *  @param cmd command line to run
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_gpssh(apr_proc_t **proc, const char *user, const char *const *hostnames, int port, const char *cmd, int blocking, apr_pool_t *p);

/**
 *  gpcc_proc_scp to run a scp remote command
 *  @param proc process handle
 *  @param user user name
 *  @param hostname
 *  @param port  ssh port number
 *  @param src source file/directory
 *  @param target  target file /directory
 *  @param wait to wait until finish
 *  @param p memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_scp(apr_proc_t *proc, const char *user, const char *hostname, int port, const char *src, const char *target, int wait, apr_pool_t *p);

/**
 *  gpcc_proc_gpscp to run scp with a list of hosts
 *  scp -pr data_dir user@host:/dir 
 *
 *  @param proc process handle
 *  @param user user name
 *  @param hostnames list of hostnames
 *  @param port  ssh port number
 *  @param src source file/directory
 *  @param target  target file /directory
 *  @param wait to wait until finish
 *  @param p memory pool
 *  @return APR_SUCCESS if success otherwise failure
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_gpscp(apr_proc_t **proc, const char *user, const char *const *hostnames, 
					  int port, const char *src, const char *target, int wait, apr_pool_t *p);



/**
 *  initdb -E encoding -D data_dir --locale=locale --max_connections=25 --shared_buffers=12000Mb
 *  
 *  @param proc process handle
 *  @param data_dir data directory
 *  @param encoding database encoding
 *  @param locale database locale
 *  @param max_connect number of max connections
 *  @param shared_buffers  shared buffers size
 *  @param blocking flag to indicate blocking or not
 *  @param p memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */

APR_DECLARE(apr_status_t) gpcc_proc_initdb(apr_proc_t *proc, const char *data_dir, const char *encoding, const char *locale, 
					   int max_connect, const char *shared_buffer, int blocking, apr_pool_t *p);
/**
 *  pg_ctl start [-w] [-D DATADIR] [-s] [-l FILENAME] [-o "OPTIONS"]
 *
 *  @param proc process handle
 *  @param port gpdb port number
 *  @param data_dir data directory
 *  @param option  postgres option
 *  @param wait flag to indicate pg_ctl wait until finish or not
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start(apr_proc_t *proc, int port, const char *data_dir, 
						 const char *option, int wait, int blocking, apr_pool_t *p);


/**
 *  pg_ctl start master in production mode
 *
 *  @param proc process handle
 *  @param port gpdb port number
 *  @param data_dir data directory
 *  @param wait flag to indicate pg_ctl wait until finish or not
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start_master(apr_proc_t *proc, int port, const char *data_dir, 
							int utility, int wait, int blocking, apr_pool_t *p);


/**
 *  pg_ctl start segment
 *
 *  @param proc process handle
 *  @param port gpdb port number
 *  @param data_dir data directory
 *  @param wait flag to indicate pg_ctl wait until finish or not
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_start_segment(apr_proc_t *proc, int port, const char *data_dir, 
							 int utility, int wait, int blocking, apr_pool_t *p);

/**
 *  gpsyncmaster [-p port] [-D DATADIR] -i
 * 
 *  @param proc process handle
 *  @param port gbdb port number
 *  @param data_dir data_directory
 *  @param option psql option
 *  @param option blocking flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 *
 */
APR_DECLARE(apr_status_t) gpcc_proc_gpsyncmaster_start(apr_proc_t *proc, int port, const char *data_dir, const char *option, int blocking, apr_pool_t *p);

/**
 *  pg_ctl stop [-W] [-D DATADIR] [-s] [-m SHUTDOWN-MODE]
 *
 *  @param proc process handle
 *  @param port gpdb port number
 *  @param data_dir data directory
 *  @param mode shutdown mode, "smart", "fast" or "immediate"
 *  @param wait flag to indicate pg_ctl wait until finish or not
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_stop(apr_proc_t *proc, int port, const char *data_dir, const char *mode, int wait, int blocking, apr_pool_t *p); 


/**
 *  pg_ctl status [-D DATADIR]
 *
 *  @param proc process handle
 *  @param port gpdb port number
 *  @param data_dir data directory
 *  @param blocking flag to indicate blocking or non-blocking
 *  @param pool memory pool
 *  @return APR_SUCCESS if success otherwise failure
 */
APR_DECLARE(apr_status_t) gpcc_proc_pg_ctl_status(apr_proc_t *proc, int port, const char *data_dir, int blocking, apr_pool_t *p);



#endif
