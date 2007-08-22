#include <stdio.h>
#include <stdlib.h>
#include <apr_hash.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <gpcc.h>
#include <gpcc_strings.h>
#include "config.h"

struct config_t {
    apr_pool_t*  pool;
    apr_hash_t*  htab;
    int          mcount;		/* modification counter */
};


config_t* config_create(apr_pool_t* pool)
{
    config_t* c = apr_pcalloc(pool, sizeof(config_t));
    GPCC_DO(0 == apr_pool_create(&c->pool, pool));
    c->htab = apr_hash_make(c->pool);
    return c;
}



void config_gc(config_t* c)
{
    apr_pool_t* npool;
    apr_hash_t* nhtab;
    GPCC_DO(0 == apr_pool_create(&npool, apr_pool_parent_get(c->pool)));
    nhtab = apr_hash_copy(npool, c->htab);
    apr_pool_destroy(c->pool);
    c->pool = npool;
    c->htab = nhtab;
    c->mcount = 0;
}




const char* config_getx(config_t* c, const char* key, const char* def)
{
    const char* v = apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
    return v ? v : def;
}


const char* config_get(config_t* c, const char* key)
{
    return (const char*) apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
}

int config_get_int(config_t* c, const char* key)
{
    const char* value = (const char*) apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
    return value ? strtol(value, 0, 0) : 0;
}



apr_int64_t config_get_int64(config_t* c, const char* key)
{
    const char* value = (const char*) apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
    return value ? strtoll(value, 0, 0) : 0;
}



const char* config_set(config_t* c, const char* key, const char* value)
{
    key = apr_pstrdup(c->pool, key);
    value = apr_pstrdup(c->pool, value);
    apr_hash_set(c->htab, key, APR_HASH_KEY_STRING, value);
    c->mcount++;
    return value;
}


int config_set_int(config_t* c, const char* key, int i)
{
    char buf[20];
    sprintf(buf, "%d", i);
    config_set(c, key, buf);
    return i;
}


apr_int64_t config_set_int64(config_t* c, const char* key, apr_int64_t i)
{
    char buf[50];
    sprintf(buf, "%lld", i);
    config_set(c, key, buf);
    return i;
}


int config_eq(config_t* c, const char* key, const char* value)
{
    const char* v = apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
    return v ? 0 == strcmp(value, v) : 0;
}


const char* config_get_or_set(config_t* c, const char* key, const char* def)
{
    const char* v = apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
    if (!v) 
	apr_hash_set(c->htab, key, APR_HASH_KEY_STRING, def);
    return v ? v : def;
}


int config_get_or_set_int(config_t* c, const char* key, int value)
{
    int v = config_get_int(c, key);
    return v ? v : config_set_int(c, key, value);
}



config_t* config_read_stream(FILE* fp, apr_pool_t* pool)
{
    char* name;
    char* value;
    config_t* cfg;
    char line[1000];
    int lno = 0;
    char* eq = 0;
    cfg = config_create(pool);
    line[sizeof(line) - 1] = 0;
    line[sizeof(line) - 2] = 0;
    while (fgets(line, sizeof(line)-1, fp)) {
	lno++;
	if (line[sizeof(line) - 2]) {
	    gpcc_log_error("config file error: line too long at line %d", lno);
	    exit(1);
	}
	name = line;
	if (strchr(name, '#')) 
	    *(strchr(name, '#')) = 0;
	if (! *(name = gpcc_string_trim(name)))
	    continue;
	if (! (eq = strchr(name, '='))) {
	    gpcc_log_error("config file error: bad syntax at line %d", lno);
	    exit(1);
	}
	*eq = 0;
	value = eq + 1;
	name = gpcc_string_trim(name);
	value = gpcc_string_trim(value);
	config_set(cfg, name, value);
    }
    return cfg;
}

config_t* config_read(const char* dirpath, const char* guid, 
		      apr_int64_t csn, apr_pool_t* pool)
{
    char pathbuf[256];
    char* path = pathbuf;
    const int pathlenmin = strlen(dirpath) + strlen(guid) + 20;
    FILE* fp;
    config_t* cfg;

    if (sizeof(pathbuf) < pathlenmin) {
	GPCC_DO(0 != (path = malloc(pathlenmin)));
    }

    sprintf(path, "%s/gpcfg_%s.%lld", dirpath, guid, csn);
    fp = fopen(path, "r");
    if (!fp) 
	return 0;
    cfg = config_read_stream(fp, pool);
    fclose(fp);
    return cfg;
}




apr_status_t config_write(const char* dirpath, const char* guid, 
			  apr_int64_t csn, const config_t* cfg)
{
    FILE* f;
    char tmpbuf[256], newbuf[256]; 
    char* tmppath = tmpbuf;
    char* newpath = newbuf;
    apr_hash_index_t* hi;
    const int pathlenmin = strlen(dirpath) + strlen(guid) + 20;

    if (sizeof(tmpbuf) < pathlenmin) {
	GPCC_DO(0 != (tmppath = malloc(pathlenmin)));
	GPCC_DO(0 != (newpath = malloc(pathlenmin)));
    }

    sprintf(tmppath, "%s/~gpcfg_%s.%lld", dirpath, guid, csn);
    sprintf(newpath, "%s/gpcfg_%s.%lld", dirpath, guid, csn);

    f = fopen(tmppath, "w");
    if (! f) {
	gpcc_fatal(FLINE, "save_config: cannot write file %s", tmppath);
    }
    for (hi = apr_hash_first(0, cfg->htab); hi; hi = apr_hash_next(hi)) {
	const char* key;
	const char* value;
	apr_hash_this(hi, (const void**) &key, 0, (void**) &value);
	fprintf(f, "%s=%s\n", key, value);
    }
    fclose(f);

    if (0 != rename(tmppath, newpath)) {
	gpcc_fatal(FLINE, "save_config: cannot rename file\n"
		       "                 from: %s\n"
		       "                   to: %s",
		       tmppath, newpath);
    }
    
    return 0;
}

apr_int64_t config_determine_csn(const char* dirpath, const char* guid)
{
    apr_pool_t* pool; 
    apr_dir_t* dir;
    apr_int64_t csn = 0;
    apr_finfo_t finfo;
    const char* prefixpath;
    int prefixlen;

    GPCC_DO(0 == apr_pool_create(&pool, 0));
    GPCC_DO(0 == apr_dir_open(&dir, dirpath, pool));

    prefixpath = apr_psprintf(pool, "%s/gpcfg_%s.", dirpath, guid);
    prefixlen = strlen(prefixpath);

    while (1) {
	switch (apr_dir_read(&finfo, APR_FINFO_NAME, dir)) {
	case APR_INCOMPLETE:
	case 0:
	    if (finfo.fname
		&& 0 == strncmp(prefixpath, finfo.fname, prefixlen)) {
		char* csnstr = strrchr(finfo.fname, '.') + 1;
		apr_int64_t ncsn;
		ncsn = strtoll(csnstr, 0, 0);
		if (ncsn > csn) 
		    csn = ncsn;
	    }
	    continue;
	default: 
	    break;
	}
	break;
    }
    apr_dir_close(dir);
    apr_pool_destroy(pool);
    return csn;
}
