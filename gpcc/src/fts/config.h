#ifndef CONFIG_H 
#define CONFIG_H

#include <apr_pools.h>

typedef struct config_t config_t;

config_t* config_create(apr_pool_t* pool);
void config_clear(config_t* c);

void config_gc(config_t* c);
int config_eq(config_t* c, const char* key, const char* value);

const char* config_getx(config_t* c, const char* key, const char* def);
const char* config_get(config_t* c, const char* key);
int config_get_int(config_t* c, const char* key);
apr_int64_t config_get_int64(config_t* c, const char* key);

const char* config_get_or_set(config_t* c, const char* key, const char* def);
int config_get_or_set_int(config_t* c, const char* key, int def);

const char* config_set(config_t* c, const char* key, const char* value);
int config_set_int(config_t* c, const char* key, int i);
apr_int64_t config_set_int64(config_t* c, const char* key, apr_int64_t i);

/* IO routiles */
apr_int64_t  config_determine_csn(const char* dirpath, const char* guid);
config_t* config_read_stream(FILE* fp, apr_pool_t* pool);
config_t* config_read(const char* dirpath, const char* guid,
		      apr_int64_t csn, apr_pool_t* pool);
apr_status_t config_write(const char* dirpath, const char* guid, 
			  apr_int64_t csn, const config_t* cfg);

#endif /* CONFIG_H*/
