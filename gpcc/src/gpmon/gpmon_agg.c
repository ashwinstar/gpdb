#undef GP_VERSION
#include "postgres_fe.h"

#include "apr_general.h"
#include "apr_hash.h"
#include "apr_time.h"
#include "gpmon/gpmon.h"
#include "gpmon_agg.h"
#include "gpmonlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>



typedef struct qenode_t qenode_t;
struct qenode_t {
    gpmon_qexec_t qexec;
    apr_int64_t cpu_elapsed;
    apr_int64_t pfaults_elapsed;
    qenode_t* next;
};

typedef struct qdnode_t qdnode_t;
struct qdnode_t {
    apr_int64_t  last_updated_generation;
    int          recorded;
    int		 qenode_recorded;
    gpmon_qlog_t qlog;
    qenode_t*    qenode_list;
};


struct agg_t {
    apr_int64_t generation;
    apr_pool_t* pool;
    apr_hash_t* qtab;		/* key = gpmon_qlog_key_t, value = qdnode ptr. */
    apr_hash_t* htab;		/* key = hostname, value = gpmon_metrics_t ptr */
};

extern int min_query_time;
extern int min_detailed_query_time;

static apr_status_t agg_put_metrics(agg_t* agg, const gpmon_metrics_t* met)
{
    gpmon_metrics_t* rec;

    rec = apr_hash_get(agg->htab, met->hname, APR_HASH_KEY_STRING);
    if (rec) {
	*rec = *met;
    }
    else {
	rec = apr_palloc(agg->pool, sizeof(*rec));
	if (!rec)
	    return APR_ENOMEM;
	*rec = *met;
	apr_hash_set(agg->htab, rec->hname, APR_HASH_KEY_STRING, rec);
    }
    return 0;
}


static apr_status_t agg_put_qlog(agg_t* agg, const gpmon_qlog_t* qlog,
				 apr_int64_t generation)
{
    qdnode_t* node;

    node = apr_hash_get(agg->qtab, &qlog->key, sizeof(qlog->key));
    if (node) {
	node->qlog = *qlog;
	if (0 != strcmp(qlog->db, GPMON_DB)) {
	    TR2(("agg_put: found %d.%d.%d generation %d recorded %d\n", qlog->key.tmid, 
		qlog->key.ssid, qlog->key.ccnt, generation, node->recorded));
	}
    }
    else {
	node = apr_palloc(agg->pool, sizeof(*node));
	if (! node)
	    return APR_ENOMEM;
	node->qlog = *qlog;
	node->recorded = 0;
	node->qenode_recorded = 0;
	node->qenode_list = 0;
	apr_hash_set(agg->qtab, &node->qlog.key, sizeof(node->qlog.key), node);
	if (0 != strcmp(qlog->db, GPMON_DB)) {
	    TR2(("agg_put: new %d.%d.%d generation %d recorded %d\n", qlog->key.tmid, 
		qlog->key.ssid, qlog->key.ccnt, generation, node->recorded));
	}
    }
    node->last_updated_generation = generation;

    return 0;
}


static apr_status_t agg_put_qexec(agg_t* agg, const gpmon_qexec_t* qexec,
				  apr_int64_t generation)
{
    qdnode_t* dp;
    gpmon_qlogkey_t key;
    int e;
    qenode_t* ep;
    qenode_t** epp;
    int diff;

    /* find qdnode of this qexec */
    key.tmid = qexec->key.tmid;
    key.ssid = qexec->key.ssid;
    key.ccnt = qexec->key.ccnt;
    dp = apr_hash_get(agg->qtab, &key, sizeof(key));
    if (!dp) {
	/* not found, make new entry */
	gpmon_qlog_t tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.key = key;
	if (0 != (e = agg_put_qlog(agg, &tmp, generation)))
	    return e;
	if (! (dp = apr_hash_get(agg->qtab, &key, sizeof(key))))
	    gpmon_fatal(FLINE, "assert failed");
    }

    /* find qexec in qenode_list */
    diff = 0;
    for (ep = dp->qenode_list, epp = &dp->qenode_list; ep; epp = &ep->next, ep = ep->next) {
	if (0 == (diff = ep->qexec.key.segid - qexec->key.segid))
	    diff = ep->qexec.key.nid - qexec->key.nid;
	if (diff >= 0) 
	    break;
    }

    /* if found, replace it */
    if (ep && diff == 0) {
	ep->qexec = *qexec;
	ep->cpu_elapsed += qexec->_cpu_elapsed;
	ep->pfaults_elapsed += qexec->_p_metrics.mem.page_faults;
    }
    else {
	qenode_t* x;
	/* not found, make new qenode */
	if (! (x = apr_palloc(agg->pool, sizeof(*x))))
	    return APR_ENOMEM;

	x->qexec = *qexec;
	x->cpu_elapsed = 0;
	x->pfaults_elapsed = 0;
	x->next = ep;
	*epp = x;
    }

    if (0) {
	int cnt = 0;
	TR2(("list now:\n"));

	for (ep = dp->qenode_list; ep; ep = ep->next) {
	    TR2(("%d: %d.%d.%s\n", ++cnt, ep->qexec.key.segid, ep->qexec.key.nid, ep->qexec.ntype));
	}
	TR2(("  -- \n"));
    }

    dp->last_updated_generation = generation;
    return 0;
}


apr_status_t agg_create(agg_t** retagg, apr_int64_t generation, 
			apr_pool_t* parent_pool)
{
    int e;
    apr_pool_t* pool;
    agg_t* agg;

    if (0 != (e = apr_pool_create(&pool, parent_pool)))
	return e;

    agg = apr_pcalloc(pool, sizeof(*agg));
    if (!agg) {
	apr_pool_destroy(pool);
	return APR_ENOMEM;
    }

    agg->generation = generation;
    agg->pool = pool;
    agg->qtab = apr_hash_make(pool);
    if (!agg->qtab) {
	apr_pool_destroy(pool);
	return APR_ENOMEM;
    }

    agg->htab = apr_hash_make(pool);
    if (!agg->htab) {
	apr_pool_destroy(pool);
	return APR_ENOMEM;
    }

    *retagg = agg;
    return 0;
}


apr_status_t agg_dup(agg_t** retagg, agg_t* oldagg, apr_pool_t* parent_pool)
{
    int e;
    agg_t* newagg;
    apr_hash_index_t* hi;

    if (0 != (e = agg_create(&newagg, oldagg->generation + 1, parent_pool))) {
	return e;
    }

    for (hi = apr_hash_first(0, oldagg->qtab); hi; hi = apr_hash_next(hi)) {
	void* vptr;
	qdnode_t* dp;
	qenode_t* ep;
	qdnode_t* newdp;
	qenode_t** tail;
	int cnt;

	apr_hash_this(hi, 0, 0, &vptr);
	dp = vptr;

	/* skip all entries that weren't updated recently */
	if (dp->last_updated_generation < newagg->generation - 1) {
	    if (0 != strcmp(dp->qlog.db, GPMON_DB)) {
		TR2(("agg_dup: skip %d.%d.%d generation %d, current generation %d, recorded %d\n", 
		    dp->qlog.key.tmid, dp->qlog.key.ssid, dp->qlog.key.ccnt,
		    (int) dp->last_updated_generation, 
		    (int) newagg->generation, dp->recorded));
	    }
	    continue;
	}

	if (0 != strcmp(dp->qlog.db, GPMON_DB)) {
	    TR2(("agg_dup: add %d.%d.%d, generation %d, recorded %d:\n", 
		 dp->qlog.key.tmid, dp->qlog.key.ssid, 
		dp->qlog.key.ccnt, dp->last_updated_generation, dp->recorded));
	}

	/* dup this entry */
	if (! (newdp = apr_palloc(newagg->pool, sizeof(*newdp)))) {
	    agg_destroy(newagg);
	    return APR_ENOMEM;
	}
	*newdp = *dp;
	newdp->qenode_list = 0;
	apr_hash_set(newagg->qtab, &newdp->qlog.key, sizeof(newdp->qlog.key), newdp);
	
	/* dup all qexec entries hanging off dp */
	cnt = 0;
	tail = &newdp->qenode_list;
	for (ep = dp->qenode_list; ep; ep = ep->next) {
	    qenode_t* x;
	    if (! (x = apr_palloc(newagg->pool, sizeof(*x)))) {
		agg_destroy(newagg);
		return APR_ENOMEM;
	    }
	    x->qexec = ep->qexec;
	    x->cpu_elapsed = ep->cpu_elapsed;
	    x->pfaults_elapsed = ep->pfaults_elapsed;
	    x->next = 0;
	    *tail = x;
	    tail = &x->next;

	    TR2(("\t    %d: (%d, %d) op %s\n", ++cnt, 
		 x->qexec.key.segid, 
		 x->qexec.key.nid,
		 x->qexec.ntype));
	}
    }

    *retagg = newagg;
    return 0;
}



void agg_destroy(agg_t* agg)
{
    apr_pool_destroy(agg->pool);
}



apr_status_t agg_put(agg_t* agg, const gpmon_packet_t* pkt)
{
    if (pkt->pkttype == GPMON_PKTTYPE_METRICS)
	return agg_put_metrics(agg, &pkt->u.metrics);
    if (pkt->pkttype == GPMON_PKTTYPE_QLOG) 
	return agg_put_qlog(agg, &pkt->u.qlog, agg->generation);
    if (pkt->pkttype == GPMON_PKTTYPE_QEXEC)
	return agg_put_qexec(agg, &pkt->u.qexec, agg->generation);

    gpmon_warning(FLINE, "unknown packet type %d", pkt->pkttype);
    return 0;
}


typedef struct bloom_t bloom_t;
struct bloom_t {
    unsigned char map[1024];
};
static void bloom_init(bloom_t* bloom);
static void bloom_set(bloom_t* bloom, const char* name);
static int  bloom_isset(bloom_t* bloom, const char* name);

static void delete_old_files(bloom_t* bloom);

static apr_status_t write_metrics(agg_t* agg, const char* nowstr);
static void write_qenode_row(FILE* fp, const qenode_t* qenode, const char* nowstr);

static void write_qlog(FILE* fp, qdnode_t *qdnode, const char* nowstr);
static void write_qlog_full(FILE* fp, qdnode_t *qdnode, const char* nowstr);

static void write_qenode_row(FILE* fp, const qenode_t* qenode, const char* nowstr)
{
    char line[1024*2];
    char qexec_tstamp[24];
    int i;
    char* p = line;
    const gpmon_qexec_t* qexec = &qenode->qexec;

    *p = 0;
    sprintf(p += strlen(p), "%s", nowstr);
    sprintf(p += strlen(p), "|%d|%d|%d|%d|%d|%d",
	    qexec->key.tmid,
	    qexec->key.ssid,
	    qexec->key.ccnt,
	    qexec->key.segid, 
	    qexec->key.pid,
	    qexec->key.nid);
    sprintf(p += strlen(p), "|%d", qexec->pnid);
    sprintf(p += strlen(p), "|%s", qexec->_hname);
    sprintf(p += strlen(p), "|%s|%s", qexec->ntype, qexec->nstatus);
    sprintf(p += strlen(p), "|%s|%d", 
	    gpmon_datetime(apr_time_from_sec(qexec->tstart), qexec_tstamp), 
	    qexec->tduration);
    sprintf(p += strlen(p), "|%" FMT64 "|%" FMT64, 
	    qexec->p_mem, qexec->p_memmax);
    sprintf(p += strlen(p), "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64,
	    qexec->_p_metrics.mem.size, 
	    qexec->_p_metrics.mem.resident, 
	    qexec->_p_metrics.mem.share, 
	    qexec->_p_metrics.mem.page_faults,
	    qenode->pfaults_elapsed);
    sprintf(p += strlen(p), "|%" FMT64 "|%.2f", 
	    qenode->cpu_elapsed,
	    qexec->_p_metrics.cpu_pct);   
    sprintf(p += strlen(p), "|%s", qexec->phase);
    for (i = 0; i < sizeof(qexec->m) / sizeof(qexec->m[0]); i++) {
	sprintf(p += strlen(p), "|%s|%s", qexec->m[i].name, 
		qexec->m[i].unit);
	sprintf(p += strlen(p), "|%" FMT64 "|%" FMT64,
		qexec->m[i].value, qexec->m[i].valuemax);
    }

    fprintf(fp, "%s\n", line);
}



apr_status_t agg_dump(agg_t* agg)
{
    apr_hash_index_t* hi;
    int e;
    bloom_t bloom;
    char nowstr[24];

    FILE* fp_iters_now = 0;
    FILE* fp_iters_tail = 0;
    FILE* fp_queries_now = 0;
    FILE* fp_queries_tail = 0;

    gpmon_datetime(apr_time_now(), nowstr);

    bloom_init(&bloom);

    /* we never delete metrics_tail/ metrics_now/ 
       queries_tail/ queries_now/ 
       iterators_tail/ iterators_now/ files */
    bloom_set(&bloom, GPMON_DIR "metrics_now.dat");
    bloom_set(&bloom, GPMON_DIR "metrics_tail.dat");
    bloom_set(&bloom, GPMON_DIR "_metrics_tail.dat");
    bloom_set(&bloom, GPMON_DIR "queries_now.dat");
    bloom_set(&bloom, GPMON_DIR "queries_tail.dat");
    bloom_set(&bloom, GPMON_DIR "_queries_tail.dat");
    bloom_set(&bloom, GPMON_DIR "iterators_now.dat");
    bloom_set(&bloom, GPMON_DIR "iterators_tail.dat");
    bloom_set(&bloom, GPMON_DIR "_iterators_tail.dat");

    /* dump metrics */
    if (0 != (e = write_metrics(agg, nowstr))) {
	goto bail;
    }

    if (! (fp_queries_tail = fopen(GPMON_DIR "queries_tail.dat", "a"))) {
	e = APR_FROM_OS_ERROR(errno);
	goto bail;
    }

    for (hi = apr_hash_first(0, agg->qtab); hi; hi = apr_hash_next(hi)) {
	void* vptr;
	qdnode_t* qdnode;
	apr_hash_this(hi, 0, 0, &vptr);
	qdnode = vptr;
	if (qdnode->qlog.status == GPMON_QLOG_STATUS_DONE || qdnode->qlog.status == GPMON_QLOG_STATUS_ERROR) {
	    if (!qdnode->recorded 
		&& qdnode->qlog.tfin - qdnode->qlog.tstart >= min_query_time) {
		/* TODO: Query text missing */
		TR1(("queries_tail: %x add query %d.%d.%d, status %d, generation %d, recorded %d\n", 
		    agg->qtab,
		    qdnode->qlog.key.tmid, qdnode->qlog.key.ssid, qdnode->qlog.key.ccnt, qdnode->qlog.status,
		    qdnode->last_updated_generation, 
		    qdnode->recorded));
                write_qlog_full(fp_queries_tail, qdnode, nowstr);
		qdnode->recorded = 1;
	    }
	}
    }
    fclose(fp_queries_tail);
    fp_queries_tail = 0;
    
    /* dump query list data */
    if (! (fp_queries_now = fopen(GPMON_DIR "_queries_now.dat", "w"))) {
        e = APR_FROM_OS_ERROR(errno);
	goto bail;
    }

    if (! (fp_iters_now = fopen(GPMON_DIR "_iterators_now.dat", "w"))) {
        e = APR_FROM_OS_ERROR(errno);
	goto bail;
    }

    if (! (fp_iters_tail = fopen(GPMON_DIR "iterators_tail.dat", "a"))) {
        e = APR_FROM_OS_ERROR(errno);
	goto bail;
    }

    for (hi = apr_hash_first(0, agg->qtab); hi; hi = apr_hash_next(hi)) {
        void* vptr;
        qdnode_t* qdnode;
        qenode_t* ep;

        apr_hash_this(hi, 0, 0, &vptr);
        qdnode = vptr;

	/* don't touch this file */
	{
	    char fname[sizeof(GPMON_DIR) + 100];
	    sprintf(fname, GPMON_DIR "q%d-%d-%d.txt",
		    qdnode->qlog.key.tmid, qdnode->qlog.key.ssid,
		    qdnode->qlog.key.ccnt);

	    bloom_set(&bloom, fname);
	}


        /* not interesting */
	/* CK: keep this for now
        if (qdnode->qenode_list == 0) 
            continue;
	*/

        /* write to _query_now.dat */
        if (qdnode->qlog.status != GPMON_QLOG_STATUS_DONE       
            && qdnode->qlog.status != GPMON_QLOG_STATUS_ERROR)
            write_qlog(fp_queries_now, qdnode, nowstr);

        /* dump query iterator data to iterators_tail */
        for (ep = qdnode->qenode_list; ep; ep = ep->next) {
	    write_qenode_row(fp_iters_now, ep, nowstr);
	}

	if (qdnode->recorded && !qdnode->qenode_recorded && 
            qdnode->qlog.tfin - qdnode->qlog.tstart >= min_detailed_query_time) {
	    for (ep = qdnode->qenode_list; ep; ep = ep->next) {
		write_qenode_row(fp_iters_tail, ep, nowstr);
	    }
	    qdnode->qenode_recorded = 1;
        }
    }

    if (fp_queries_now) fclose(fp_queries_now);
    if (fp_iters_now) fclose(fp_iters_now);
    if (fp_iters_tail) fclose(fp_iters_tail);
    if (fp_queries_tail) fclose(fp_queries_tail);
    rename(GPMON_DIR "_metrics_now.dat", GPMON_DIR "metrics_now.dat");
    rename(GPMON_DIR "_iterators_now.dat", GPMON_DIR "iterators_now.dat");
    rename(GPMON_DIR "_queries_now.dat", GPMON_DIR "queries_now.dat");

    /* clean up ... delete all old files by checking our bloom filter */
    delete_old_files(&bloom);
    
    return 0;

    bail:
    if (fp_queries_now) fclose(fp_queries_now);
    if (fp_queries_tail) fclose(fp_queries_tail);
    if (fp_iters_now) fclose(fp_iters_now);
    if (fp_iters_tail) fclose(fp_iters_tail);
    return e;
}

extern int gpmmon_quantum(void);

static void delete_old_files(bloom_t* bloom)
{
    FILE* fp = popen("ls -1 " GPMON_DIR "q*-*.txt 2> /dev/null", "r");
    time_t cutoff = time(0) - gpmmon_quantum() * 3;
    if (fp) {
	for (;;) {
	    char line[1024];
	    char* p;
	    struct stat stbuf;

	    line[sizeof(line) - 1] = 0;
	    if (! (p = fgets(line, sizeof(line), fp))) 
		break;
	    if (line[sizeof(line) - 1]) 
		continue; 	/* fname too long */
	    
	    p = gpmon_trim(p);
	    if (bloom_isset(bloom, p))
		continue;	/* probably one of our files */
	    
	    if (0 == stat(p, &stbuf)) {
		//printf("file %s mtime %d cutoff %d ", p, (int)stbuf.st_mtimespec.tv_sec, (int)cutoff);
#if defined(sun) || defined(linux)
		int expired = stbuf.st_mtime < cutoff;
#else
		int expired = stbuf.st_mtimespec.tv_sec < cutoff;
#endif
		if (expired) {
		    //printf(" ---> deleting");
		    unlink(p);
		}
		//printf("\n");
	    }
	}
	pclose(fp);
    }
}


static apr_status_t write_metrics(agg_t* agg, const char* nowstr)
{
    FILE* fp = fopen(GPMON_DIR "metrics_tail.dat", "a");
    FILE* fp2 = fopen(GPMON_DIR "_metrics_now.dat", "w");
    int e;
    apr_hash_index_t* hi;
    char line[1000];
    char* p;

    if (!fp || !fp2) {
	e = APR_FROM_OS_ERROR(errno);
	if (fp) fclose(fp);
	if (fp2) fclose(fp2);
	return e;
    }

    for (hi = apr_hash_first(0, agg->htab); hi; hi = apr_hash_next(hi)) {
	gpmon_metrics_t* mp;
	void* valptr = 0;
        int quantum = gpmmon_quantum();
	apr_hash_this(hi, 0, 0, (void**) &valptr);

	mp = (gpmon_metrics_t*) valptr;

	p = line; line[0] = 0;
	sprintf(p += strlen(p), "%s|%s", 
		nowstr, mp->hname);
	sprintf(p += strlen(p), "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64,
		mp->mem.total, mp->mem.used, mp->mem.actual_used, mp->mem.actual_free);
	sprintf(p += strlen(p), "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64,
		mp->swap.total, mp->swap.used, mp->swap.page_in, mp->swap.page_out);
	sprintf(p += strlen(p), "|%.2f|%.2f|%.2f", 
		mp->cpu.user_pct, mp->cpu.sys_pct, mp->cpu.idle_pct);
	sprintf(p += strlen(p), "|%.4f|%.4f|%.4f",
		mp->load_avg.value[0], mp->load_avg.value[1], mp->load_avg.value[2]);
        sprintf(p += strlen(p), "|%d", quantum); 
	sprintf(p += strlen(p), "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64,
		mp->disk.rcount / quantum, mp->disk.wcount / quantum, 
                mp->disk.rbytes / quantum, mp->disk.wbytes / quantum);
	sprintf(p += strlen(p), "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64,
		mp->net.rcount / quantum, mp->net.wcount / quantum, 
                mp->net.rbytes / quantum, mp->net.wbytes / quantum);
	fprintf(fp, "%s\n", line);
	fprintf(fp2, "%s\n", line);
    }
    fclose(fp);
    fclose(fp2);
    return 0;
}

static apr_int64_t get_total_cpu_elapsed(qdnode_t* qdnode)
{
    qenode_t* pqe = NULL;
    apr_int64_t total_cpu_elapsed = 0;
    
    for (pqe = qdnode->qenode_list; pqe; pqe = pqe->next)
        total_cpu_elapsed += pqe->cpu_elapsed;

    return total_cpu_elapsed;
}

static float get_cpu_current_perc(qdnode_t* qdnode)
{
    apr_pool_t* tmp_pool;
    apr_hash_t* segtab;
    apr_hash_t* segpidtab;
    apr_hash_index_t *hi;

    qenode_t* pqe = NULL;

    void* valptr;

    float* seg_cpu_current, cpu_avg = 0.0f;
    int segcnt = 0;
    int e;
    char key[32];
    int dummyval = 1;

    if (!qdnode)
        return 0.0f;

    if (0 != (e = apr_pool_create(&tmp_pool, 0)))
        gpmon_fatalx(FLINE, e, "apr_pool_create failed");

    segtab = apr_hash_make(tmp_pool);
    if (!segtab)
        gpmon_fatal(FLINE, "Out of memory");
 
    segpidtab = apr_hash_make(tmp_pool);
    if (!segpidtab)
        gpmon_fatal(FLINE, "Out of memory"); 

    /* Calc mean per segment */
    for (pqe = qdnode->qenode_list; pqe; pqe = pqe->next)
    {
        /* Skip QD */
        if (pqe->qexec.key.segid == -1)
            continue;

        memset(key, 0, 32);
        snprintf(key, 32, "%d%d", pqe->qexec.key.segid, pqe->qexec.key.pid); 

        seg_cpu_current = apr_hash_get(segpidtab, key, sizeof(key));
        
        if (!seg_cpu_current) 
        {
            seg_cpu_current = apr_palloc(tmp_pool, sizeof(float));
            *seg_cpu_current = pqe->qexec._p_metrics.cpu_pct;
            apr_hash_set(segpidtab, key, sizeof(key), seg_cpu_current);

            valptr = apr_hash_get(segtab, &pqe->qexec.key.segid, sizeof(pqe->qexec.key.segid));
            if (valptr == NULL)
            {
                segcnt++;
                apr_hash_set(segtab, &pqe->qexec.key.segid, 
                             sizeof(pqe->qexec.key.segid), &dummyval);
            }
        }
    }
    /* Calc mean across all segments */
    for (hi = apr_hash_first(NULL, segpidtab); hi; hi = apr_hash_next(hi))
    {
        apr_hash_this(hi, 0, 0, &valptr);
        seg_cpu_current = (float*) valptr;
        cpu_avg += *seg_cpu_current;
    }
    
    if (!segcnt) {
        TR2(("No segments for CPU current calculation\n"));
        apr_pool_destroy(tmp_pool);
        return 0.0f;
    }

    cpu_avg = cpu_avg / (float)segcnt;

    apr_pool_destroy(tmp_pool);
    
    return cpu_avg;
}

static double get_cpu_skew(qdnode_t* qdnode)
{
    apr_pool_t* tmp_pool;
    apr_hash_t* segtab;
    apr_hash_index_t *hi;

    qenode_t* pqe = NULL;

    apr_int64_t cpu_avg = 0;
    apr_int64_t* seg_cpu_sum = NULL;
    void* valptr;

    double var = 0.0f;

    int segcnt = 0;
    int e;

    if (!qdnode)
        return 0.0f;

    if (0 != (e = apr_pool_create(&tmp_pool, 0)))
        gpmon_fatalx(FLINE, e, "apr_pool_create failed");

    segtab = apr_hash_make(tmp_pool);
    if (!segtab)
        gpmon_fatal(FLINE, "Out of memory"); 

    /* Calc mean per segment */
    for (pqe = qdnode->qenode_list; pqe; pqe = pqe->next)
    {
        /* Skip QD */
        if (pqe->qexec.key.segid == -1)
            continue;
        
        seg_cpu_sum = apr_hash_get(segtab, &pqe->qexec.key.segid,
                                   sizeof(pqe->qexec.key.segid));
        
        if (!seg_cpu_sum) {
            seg_cpu_sum = apr_palloc(tmp_pool, sizeof(apr_int64_t));
            *seg_cpu_sum = 0;
        }

        *seg_cpu_sum += pqe->cpu_elapsed;
        TR2(("(SKEW) Iterator data for seg %d.  CPU Elapsed: %" FMT64 "\n",
             pqe->qexec.key.segid, pqe->cpu_elapsed));
        apr_hash_set(segtab, &pqe->qexec.key.segid, 
                     sizeof(pqe->qexec.key.segid), seg_cpu_sum);
    }

    /* Calc mean across all segments */
    for (hi = apr_hash_first(NULL, segtab); hi; hi = apr_hash_next(hi))
    {
        apr_hash_this(hi, 0, 0, &valptr);
	seg_cpu_sum = (apr_int64_t*) valptr;
        cpu_avg += *seg_cpu_sum;
        TR2(("(SKEW) Segment rusage: %d\n", *seg_cpu_sum));
        segcnt++;
    }
    
    if (!segcnt) {
        TR2(("No segments for CPU skew calculation\n"));
        apr_pool_destroy(tmp_pool);
        return 0.0f;
    }

    cpu_avg = cpu_avg / segcnt;
    TR2(("(SKEW) Avg rusage: %" FMT64 "\n", cpu_avg));

    /* Calc sqrt of dev squared mean */
    for (hi = apr_hash_first(NULL, segtab); hi; hi = apr_hash_next(hi))
    {
        apr_int64_t dev = 0;

        apr_hash_this(hi, NULL, NULL, &valptr);
	seg_cpu_sum = (apr_int64_t*) valptr;
        dev = *seg_cpu_sum - cpu_avg;
        TR2(("(SKEW) Deviation: %d\n", dev)); 
        var += dev * dev;
    }
  
    var = sqrt(var / (double)segcnt);
  
    TR2(("(SKEW) CPU variance: %f\n", var));

    /* Skew calc */
    apr_pool_destroy(tmp_pool);
    TR2(("(SKEW) CPU Skew: %f\n", cpu_avg ? var/(double)cpu_avg : 0.0f));

    return (cpu_avg ? (var/(double)cpu_avg) * 100.0f : 0.0f);
}

static double get_row_skew(qdnode_t* qdnode)
{

    apr_pool_t* tmp_pool;
    apr_hash_t* segtab;
    apr_hash_index_t *hi;

    qenode_t* pqe = NULL;

    apr_int64_t row_in_avg = 0;
    apr_int64_t* seg_row_in_sum = NULL;
    void* valptr;

    double var = 0.0f;

    int segcnt = 0;
    int e;

    if (!qdnode)
        return 0.0f;

    if (0 != (e = apr_pool_create(&tmp_pool, 0)))
        gpmon_fatalx(FLINE, e, "apr_pool_create failed");

    segtab = apr_hash_make(tmp_pool);
    if (!segtab)
        gpmon_fatal(FLINE, "Out of memory"); 

    /* Calc rows in sum per segment */
    for (pqe = qdnode->qenode_list; pqe; pqe = pqe->next)
    {
        /* Skip QD */
        if (pqe->qexec.key.segid == -1)
            continue;
        
        seg_row_in_sum = apr_hash_get(segtab, &pqe->qexec.key.segid,
                                   sizeof(pqe->qexec.key.segid));
        
        if (!seg_row_in_sum) {
            seg_row_in_sum = apr_palloc(tmp_pool, sizeof(apr_int64_t));
            *seg_row_in_sum = 0;
        }

        *seg_row_in_sum += pqe->qexec.m[GPMON_QEXEC_M_ROWSIN].value;
        TR2(("(SKEW) Iterator data for seg %d.  Rows in: %" FMT64 "\n",
             pqe->qexec.key.segid, pqe->qexec.m[GPMON_QEXEC_M_ROWSIN].value));
        apr_hash_set(segtab, &pqe->qexec.key.segid, 
                     sizeof(pqe->qexec.key.segid), seg_row_in_sum);
    }

    /* Calc rows in mean across all segments */
    for (hi = apr_hash_first(NULL, segtab); hi; hi = apr_hash_next(hi))
    {
        apr_hash_this(hi, 0, 0, &valptr);
	seg_row_in_sum = (apr_int64_t*) valptr;
        row_in_avg += *seg_row_in_sum;
        segcnt++;
    }

    if (!segcnt) {
        TR2(("No segments for CPU skew calculation\n"));
        apr_pool_destroy(tmp_pool);
        return 0.0f;
    }

    row_in_avg = row_in_avg / segcnt;

    TR2(("(SKEW) Avg rows in: %" FMT64 "\n", row_in_avg));

    /* Calc sqrt of dev squared mean */
    for (hi = apr_hash_first(NULL, segtab); hi; hi = apr_hash_next(hi))
    {
        apr_int64_t dev = 0;

        apr_hash_this(hi, NULL, NULL, &valptr);
	seg_row_in_sum = (apr_int64_t*) valptr;
        dev = *seg_row_in_sum - row_in_avg;
        TR2(("(SKEW) Deviation: %d\n", dev)); 
        var += dev * dev;
    }

    var = sqrt(var / (double)segcnt);
    
    TR2(("(SKEW) Rows in variance: %f\n", var));

    /* Skew calc */
    apr_pool_destroy(tmp_pool);
    TR2(("(SKEW) Rows in skew: %f\n", row_in_avg ? var/(double)row_in_avg : 0.0f));

    return (row_in_avg ? (var/(double)row_in_avg) * 100.0f : 0.0f);
}


static char* fmt_qlog(char* line, qdnode_t* qdnode, const char* nowstr)
{
    char tmstr[24];
    char* p = line;
    double cpu_skew = 0.0f;
    double row_skew = 0.0f;
    apr_int64_t cpu_elapsed = 0;
    float cpu_current = 0.0f;

    *p = 0;
    sprintf(p += strlen(p), "%s", nowstr);
    sprintf(p += strlen(p), "|%d|%d|%d", qdnode->qlog.key.tmid, 
            qdnode->qlog.key.ssid, qdnode->qlog.key.ccnt);
    sprintf(p += strlen(p), "|%s|%s", qdnode->qlog.user, 
            qdnode->qlog.db);
    sprintf(p += strlen(p), "|%d", qdnode->qlog.cost);
    sprintf(p += strlen(p), "|%s", 
            gpmon_datetime(apr_time_from_sec(qdnode->qlog.tsubmit), 
                             tmstr));
    sprintf(p += strlen(p), "|%s", 
            gpmon_datetime(apr_time_from_sec(qdnode->qlog.tstart), 
                             tmstr));
    sprintf(p += strlen(p), "|%s", 
            gpmon_datetime(apr_time_from_sec(qdnode->qlog.tfin), 
                             tmstr));
    sprintf(p += strlen(p), "|%s", 
            gpmon_qlog_status_string(qdnode->qlog.status));
    sprintf(p += strlen(p), "|%" FMT64, qdnode->qlog.rows_out);
    cpu_elapsed = get_total_cpu_elapsed(qdnode);
    cpu_current = get_cpu_current_perc(qdnode);
    sprintf(p += strlen(p), "|%" FMT64 "|%.4f", 
            cpu_elapsed, cpu_current);

    cpu_skew = get_cpu_skew(qdnode);
    row_skew = get_row_skew(qdnode);

    sprintf(p += strlen(p), "|%.2f|%.2f", cpu_skew, row_skew);
    sprintf(p += strlen(p), "|%d", 0);
    return line;
}


static void write_qlog(FILE* fp, qdnode_t *qdnode, const char* nowstr)
{
    char line[1024];
    char* p = fmt_qlog(line, qdnode, nowstr);
    /* Query text "joined" by python script */
    fprintf(fp, "%s||\n", p);
}

static void write_qlog_full(FILE* fp, qdnode_t *qdnode, const char* nowstr)
{
    char line[1024];
    char* p;
    char qfname[100];
    FILE* qfptr = 0;
    int qtextlen = 0;
    
    fprintf(fp, "%s", fmt_qlog(line, qdnode, nowstr));
    fprintf(fp, "|");
    sprintf(qfname, GPMON_DIR "q%d-%d-%d.txt", qdnode->qlog.key.tmid, 
            qdnode->qlog.key.ssid, qdnode->qlog.key.ccnt);
    qfptr = fopen(qfname, "r");
    if (qfptr) {
        line[sizeof(line)-1] = 0;
        p = fgets(line, sizeof(line), qfptr);
        if (p && line[sizeof(line)-1]) {
            line[sizeof(line)-1] = 0;
            gpmon_warning(FLINE, "line too long (ignored): %s\n", line);
            p = 0;
        }
        if (p && 1 != sscanf(p, "%d", &qtextlen)) {
            gpmon_warning(FLINE, "illegal line in query text file: %s\n", line);
            p = 0;
            qtextlen = 0;
        }

	if (qtextlen > 0) {
	    fprintf(fp, "\"");
        
	    while (qtextlen > 0) {
		int max, n;
		char* q;
		max = qtextlen > sizeof(line) ? sizeof(line) : qtextlen;
		n = fread(line, 1, max, qfptr);
		for (p = line, q = line + n; p < q; p++) {
		    if (*p == '"')
			fputc('\"', fp);
		    fputc(*p, fp);
		}
		qtextlen -= n;
		if (n < max) break;
	    }

	    fprintf(fp, "\"");
	}
        fclose(qfptr);
    }
    else {
        TR1(("Failed to open query text file: %s\n", qfname));
    }
    fprintf(fp, "|\n");
}

static void bloom_init(bloom_t* bloom)
{
    memset(bloom->map, 0, sizeof(bloom->map));
}

static void bloom_set(bloom_t* bloom, const char* name)
{
    apr_ssize_t namelen = strlen(name);
    const unsigned int hashval = 
	apr_hashfunc_default(name, &namelen) % (8 * sizeof(bloom->map));
    const int idx = hashval / 8;
    const int off = hashval % 8;
    /* printf("bloom set %s h%d\n", name, hashval); */
    bloom->map[idx] |= (1 << off);
}

static int bloom_isset(bloom_t* bloom, const char* name)
{
    apr_ssize_t namelen = strlen(name);
    const unsigned int hashval = 
	apr_hashfunc_default(name, &namelen) % (8 * sizeof(bloom->map));
    const int idx = hashval / 8;
    const int off = hashval % 8;
    /*
      printf("bloom check %s h%d = %d\n", name, hashval,
      0 != (bloom->map[idx] & (1 << off)));
    */
    return 0 != (bloom->map[idx] & (1 << off));
}


