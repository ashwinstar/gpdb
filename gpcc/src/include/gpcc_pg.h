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

/**
 *  get qe details
 *  @param port port number
 */
APR_DECLARE(apr_status_t) gpcc_pg_get_qe_details(int port, apr_pool_t *p) ;

/**
 *  change password
 *  @param port Greenplum Database port number
 *  @param user username
 *  @param password password
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_change_password(int port, const char *user, const char *password, apr_pool_t *p) ;


/**
 *  create database
 *  @param port Greenplum Database port number
 *  @param dbname database name
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_create_database(int port, const char *dbname, apr_pool_t *p) ;


/**
 *  update gp_configuration
 *
 *  @param port  database port number
 *  @param dbid  dbid
 *  @param content
 *  @param primary
 *  @param hostname
 *  @param seg_port   segment port number
 *  @param data_dir   segment data directory
 *  @param p memory pool
 *  @return APR_SUCCESS if success 
 */
APR_DECLARE(apr_status_t) gpcc_pg_update_gp_configuration(int port, 
							  int dbid,
							  int content,
							  const char *primary,
							  const char *hostname,
							  int seg_port,
							  const char *data_dir,
							  apr_pool_t *p);


/**
 *  update gp_id
 *
 *  @param gpname Greenplum Database name
 *  @param numseg number of segments
 *  @param dbid
 *  @param content
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */
APR_DECLARE(apr_status_t) gpcc_pg_update_gp_id(int port, 
					       const char *gpname,
					       int numseg,
					       int dbid,
					       int content,
					       apr_pool_t *p);


typedef struct gpcc_pg_conf_subst_t gpcc_pg_conf_subst_t;
struct gpcc_pg_conf_subst_t {			
    char *pattern;
    char *insert;
    int inserted;
};		       

/**
 *  update postgresql.conf
 *  @param data_dir data directory
 *  @param subst pattern to substitue or insert.  (NULL terminated)
 *  @param p memory pool
 *  @return APR_SUCCESS if success
 */

APR_DECLARE(apr_status_t) gpcc_pg_update_postgresql_conf(const char *data_dir, gpcc_pg_conf_subst_t *subst, apr_pool_t *p);


#endif   /* GPCC_PG_H */
