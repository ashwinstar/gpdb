#ifndef CONFIG_H 
#define CONFIG_H

#include <stdio.h>
#include <apr_pools.h>

typedef struct config_t config_t;

/**
 *  Delete any config file in dirpath.
 */
void config_clean_dir(const char* dirpath, apr_pool_t* pool);

/**
 *  Create a new configuration.
 */
config_t* config_create(apr_pool_t* pool);

/**
 *  Reset the configuration to empty.
 */
void config_clear(config_t* c);

/**
 *  Garbage collect the configuration.
 */
void config_gc(config_t* c);

/**
 *  Check that a key in the configuration is equal to value.
 */
int config_eq(config_t* c, const char* key, const char* value);

/**
 *  Get the value of a key. If not set, then return the default 'def'.
 */
const char* config_getx(const config_t* c, const char* key, const char* def);

/**
 *  Get the value of a key. If not set, return NULL.
 */
const char* config_get(const config_t* c, const char* key);

/**
 *  Get the integer value of a key. If not set, return 0.
 */
int config_get_int(const config_t* c, const char* key);

/**
 *  Get the 64-bit integer value of a key. If not set, return 0.
 */
apr_int64_t config_get_int64(const config_t* c, const char* key);

/**
 *  Get the value of a key. If not set, set it to the default 'def', and return it.
 */
const char* config_get_or_set(config_t* c, const char* key, const char* def);

/**
 *  Get the integer value of a key. If not set, set it to the 'def', and return it.
 */
int config_get_or_set_int(config_t* c, const char* key, int def);

/**
 *  Set or insert a new (key, value).
 */
const char* config_set(config_t* c, const char* key, const char* value);

/**
 *  Set the value of a key.
 */
int config_set_int(config_t* c, const char* key, int i);

/**
 *  Set the value of a key.
 */
apr_int64_t config_set_int64(config_t* c, const char* key, apr_int64_t i);

/**
 *  Read the config from a file stream.
 */
config_t* config_read_stream(FILE* fp, apr_pool_t* pool);

/**
 *  Read the config from a directory. Routine will find all files in dirpath
 *  matching the config file pattern, determine the one with the largest CSN, 
 *  read that file, and return the configuration.
 */
config_t* config_read(const char* dirpath, apr_pool_t* pool);

/**
 *  Write the config to the dirpath. Routine will increment the CSN value, 
 *  and write a new file to dirpath atomically.
 */
apr_status_t config_write(const char* dirpath, const char* guid, config_t* cfg);

#endif /* CONFIG_H*/
