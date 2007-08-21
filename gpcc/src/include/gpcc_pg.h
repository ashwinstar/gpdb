#ifndef GPCC_PG_H
#define GPCC_PG_H

#include <gpcc.h>

/**
 * pg_ctl status
 *
 * @param port gp_port
 * @param data_dir data directory
 * @param p memory pool
 * @return APR_SUCCESS if server is running
 */
APR_DECLARE(apr_status_t) gpcc_pg_ctl_status(int port, const char *data_dir, apr_pool_t *p);



/**
 *  append pg_hba.conf
 *  @param data_dir data directory
 *  @param hosts list of hostname and terminate with NULL pointer
 *  @param p memory pool
 *  @return APR_SUCCES if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_hba_append(const char *data_dir, const char *const *hosts, apr_pool_t *p);

/**
 *  rotate postgres log
 *  @param data_dir data directory
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_log_rotate(const char *data_dir, apr_pool_t *p);

/**
 *  get active postgres pid
 *  @param port gp_port
 *  @param pid return process id.  pid = 1 if process is running but no lock file found.
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_pid_active(int port, int *pid, apr_pool_t *p);


/**
 *  reset the invalid segment in gp_configuration table 
 *  @param master_port master port number
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_reset_invalid_segment(int master_port, apr_pool_t *p);


/**
 *  check the segment
 *  @param port segment port number
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_check_segment(int port, apr_pool_t *p);

#endif
