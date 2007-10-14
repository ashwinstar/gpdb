#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <apr_hash.h>
#include <apr_fnmatch.h>
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
    if (apr_pool_create(&c->pool, pool))
	gpcc_fatal(FLINE, "out of memory");
    c->htab = apr_hash_make(c->pool);
    return c;
}



void config_gc(config_t* c)
{
    apr_pool_t* npool;
    apr_hash_t* nhtab;
    if (apr_pool_create(&npool, apr_pool_parent_get(c->pool)))
	gpcc_fatal(FLINE, "out of memory");
    nhtab = apr_hash_copy(npool, c->htab);
    apr_pool_destroy(c->pool);
    c->pool = npool;
    c->htab = nhtab;
    c->mcount = 0;
}




const char* config_getx(const config_t* c, const char* key, const char* def)
{
    const char* v = apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
    return v ? v : def;
}


const char* config_get(const config_t* c, const char* key)
{
    return (const char*) apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
}

int config_get_int(const config_t* c, const char* key)
{
    const char* value = (const char*) apr_hash_get(c->htab, key, APR_HASH_KEY_STRING);
    return value ? strtol(value, 0, 0) : 0;
}



apr_int64_t config_get_int64(const config_t* c, const char* key)
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
    sprintf(buf, "%lld", (long long) i);
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


static char* path_basename(const char* path, apr_pool_t* pool)
{
    char* idx = strrchr(path, '/');
    if (! idx) 
	return apr_pstrdup(pool, ".");

    return apr_pstrndup(pool, path, idx - path);
}


static char** glob(char* path, int* retcnt, apr_pool_t* pool)
{
    apr_array_header_t* arr;
    char** elts;
    int i, n;
    char** fpath = 0;
    char* bname = path_basename(path, pool);
    char** p;

    if (apr_match_glob(path, &arr, pool)) 
	return 0;
    n = arr->nelts;
    fpath = apr_pcalloc(pool, sizeof(char*) * (n + 1));
    elts = (char**) arr->elts;
    p = fpath;
    for (i = 0; i < n; i++) {
	if (apr_filepath_merge(p, bname, elts[i], 0, pool))
	    return 0;
	p++;
    }
    
    if (retcnt) 
	*retcnt = p - fpath;


    return fpath;
}

config_t* config_read(const char* dirpath, apr_pool_t* pool)
{
    apr_pool_t* subpool; 
    config_t* cfg = 0;
    int i, fcnt = 0;
    char** fname = 0;
    char* path;
    char* maxfile = 0;
    apr_int64_t csn = 0;
    FILE* fp;

    if (apr_pool_create(&subpool, pool)) 
	gpcc_fatal(FLINE, "out of memory");

    /* get a list of all filenames in dirpath matching gpcfg_* */
    path = apr_psprintf(subpool, "%s/gpcfg_*", dirpath);
    fname = glob(path, &fcnt, subpool);

    /* go thru the list to determine biggest csn */
    for (i = 0; i < fcnt; i++) {
	char* csnstr = strrchr(fname[i], '.');
	apr_int64_t ncsn;
	if (!csnstr) continue;
	*csnstr++ = 0;
	ncsn = strtoll(csnstr, 0, 0);
	if (maxfile && 0 != strcmp(maxfile, fname[i])) 
	    gpcc_fatal(FLINE, "found config files of multiple GUID in dir %s", dirpath);
	if (ncsn > csn) {
	    maxfile = fname[i];
	    csn = ncsn;
	}
    }

    if (maxfile) {
	/* reconstruct the fname */
	maxfile = apr_psprintf(subpool, "%s.%lld", maxfile, (long long) csn);
	fp = fopen(maxfile, "r");
	if (fp) 
	    cfg = config_read_stream(fp, pool);
	fclose(fp);
    }
    
    apr_pool_destroy(subpool);
    return cfg;
}


apr_status_t config_write(const char* dirpath, const char* guid, 
			  config_t* cfg)
{
    FILE* f;
    char tmpbuf[256], newbuf[256]; 
    char* tmppath = tmpbuf;
    char* newpath = newbuf;
    apr_hash_index_t* hi;
    apr_int64_t csn;
    const int pathlenmin = strlen(dirpath) + strlen(guid) + 20;

    csn = config_get_int64(cfg, "sys.csn") + 1;
    config_set_int64(cfg, "sys.csn", csn);

    if (sizeof(tmpbuf) < pathlenmin) {
	if (! (tmppath = malloc(pathlenmin)))
	    gpcc_fatal(FLINE, "out of memory");

	if (! (newpath = malloc(pathlenmin)))
	    gpcc_fatal(FLINE, "out of memory");
    }

    sprintf(tmppath, "%s/~gpcfg_%s.%lld", dirpath, guid, (long long) csn);
    sprintf(newpath, "%s/gpcfg_%s.%lld", dirpath, guid, (long long) csn);

    f = fopen(tmppath, "w");
    if (! f) {
	gpcc_fatal(FLINE, "save_config: cannot write file %s", tmppath);
    }
    for (hi = apr_hash_first(0, cfg->htab); hi; hi = apr_hash_next(hi)) {
	const void* k;
	void* v;
	const char* key;
	const char* value;
	apr_hash_this(hi, &k, 0, &v);
	key = k;
	value = v;
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


void config_clean_dir(const char* dirpath, apr_pool_t* pool)
{
    apr_pool_t* subpool;
    char** fname = 0;
    char* path;
    int fcnt, i;
    
    if (apr_pool_create(&subpool, pool)) 
	gpcc_fatal(FLINE, "out of memory");

    /* get a list of all filenames in dirpath matching gpcfg_* */
    path = apr_psprintf(subpool, "%s/gpcfg_*", dirpath);
    fname = glob(path, &fcnt, subpool);

    /* go thru the list and delete one by one. */
    for (i = 0; i < fcnt; i++) {
	if (unlink(fname[i]) == -1) {
	    gpcc_fatal(FLINE, "unable to delete file %s", fname[i]);
	}
    }
}
