#ifndef GPMONDB_H
#define GPMONDB_H

#include "apr_general.h"
#include "apr_md5.h"

/**
 *  Retrieve the gpmon_port from GPDB. (SHOW GPMON_PORT)
 */
APR_DECLARE(int) gpdb_get_gpmon_port(void);

/**
 *  Retrieve a list of all hosts in the GPDB.
 *  @param hostcnt return # elements in hostvec
 *  @param hostvec return array of hostnames.
 *  @param pool where to allocate hostvec and its contents.
 */
APR_DECLARE(void) gpdb_get_hostlist(int* hostcnt, char*** hostvec, apr_pool_t* pool);

/**
 *  Get the MD5 of the gp_configuration
 */
APR_DECLARE(void) gpdb_get_config_md5(unsigned char md5[APR_MD5_DIGESTSIZE]);

/**
 *  Get the master data directory in the GPDB.
 *  @param mstrdir return the master data directory
 *  @param hostname return the master hostname
 *  @param pool where to allocate hostname and mstrdir
 */
APR_DECLARE(void) gpdb_get_master_data_dir(char** hostname, char** mstrdir, apr_pool_t* pool);

/**
 *
 */
APR_DECLARE(void) gpdb_harvest(void);


#endif /* GPMONDB_H */
