#include <gpcc_fsm.h>
#include <apr_hash.h>
#include <stdarg.h>                    
#include <stdio.h>                     
                                       
typedef struct fsmrec_t fsmrec_t;      
struct fsmrec_t {
    struct { int curstate, evtype; } key;
    int nxtstate;
    gpcc_fsm_func_t** func;
};
    
    
typedef struct fsm_t fsm_t;
struct fsm_t {
    apr_pool_t* pool;
    apr_hash_t* htab;
};  
    

static fsm_t fsm = {0};
    
    
    
apr_status_t gpcc_fsm_put(int curstate,
                                       int evtype,
                                       int nxtstate,
                                       gpcc_fsm_func_t* func, ...)
{
    va_list ap;                        
    fsmrec_t* rec;                     

    int cnt, i;

    /* initialized ? */
    if (! fsm.pool) {
        GPCC_DO(0 == apr_pool_create(&fsm.pool, 0));
        fsm.htab = apr_hash_make(fsm.pool);
    }

    /* allocate a fsm record */
    rec = apr_pcalloc(fsm.pool, sizeof(fsmrec_t));

    /* count #func by scanning for NUL */
    va_start(ap, func);
    for (i = 1; i < 100 && va_arg(ap, gpcc_fsm_func_t*); i++);
    if (i >= 100) return APR_EINVAL;
    cnt = i + 1;
    va_end(ap);

    /* fill up the rec */
    rec->key.curstate = curstate;
    rec->key.evtype = evtype;
    rec->nxtstate = nxtstate;
    rec->func = apr_pcalloc(fsm.pool, sizeof(gpcc_fsm_func_t*) * cnt);
    va_start(ap, func);
    rec->func[0] = func;
    for (i = 1; i < cnt; i++) {
        rec->func[i] = va_arg(ap, gpcc_fsm_func_t*);
    }

    /* insert into htab */
    apr_hash_set(fsm.htab, &rec->key, sizeof(rec->key), rec);

    return 0;
}


apr_status_t gpcc_fsm_get(int curstate, int evtype,
			  int* pnxtstate,
			  gpcc_fsm_func_t*** pfunc)
{
    fsmrec_t* rec;
    fsmrec_t tmp;

    tmp.key.curstate = curstate;
    tmp.key.evtype = evtype;

    rec = (fsmrec_t*) apr_hash_get(fsm.htab, &tmp.key, sizeof(tmp.key));
    if (!rec) {
        *pnxtstate = 0;
        *pfunc = 0;
        return APR_EINVAL;
    }
    
    *pnxtstate = rec->nxtstate;
    *pfunc = rec->func;
    return 0;
}


