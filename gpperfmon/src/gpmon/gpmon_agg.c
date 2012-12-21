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
    qenode_t* next;
};

typedef struct qdnode_t qdnode_t;
struct qdnode_t {
    apr_int64_t  last_updated_generation;
    int          recorded;
    int		 qenode_recorded;
    int          num_metrics_packets;
    gpmon_qlog_t qlog;
    qenode_t*    qenode_list;
};

typedef struct gpmon_filrep_holder_t gpmon_filrep_holder_t;
struct gpmon_filrep_holder_t
{
	gpmmon_filerep_key_t key;
	double elapsedTime_secs_primary;
	double elapsedTime_secs_mirror;
	gpmon_filerep_primarystats_s primary;
	gpmon_filerep_mirrorstats_s mirror;
};


struct agg_t 
{
	apr_int64_t generation;
	apr_pool_t* pool;
	apr_hash_t* qtab;		/* key = gpmon_qlog_key_t, value = qdnode ptr. */
	apr_hash_t* htab;		/* key = hostname, value = gpmon_metrics_t ptr */
	apr_hash_t* stab;		/* key = databaseid, value = gpmon_seginfo_t ptr */
	apr_hash_t* ftab;		/* key = gpmmon_filerep_key_t, value = gpmon_filrep_holder_t ptr */
};

typedef struct dbmetrics_t dbmetrics_t;
struct dbmetrics_t {
    apr_int32_t queries_total;
    apr_int32_t queries_running;
    apr_int32_t queries_queued;
};

extern int min_query_time;
extern int min_detailed_query_time;

extern void incremement_tail_bytes(apr_uint64_t bytes);

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

static apr_status_t agg_put_segment(agg_t* agg, const gpmon_seginfo_t* seg)
{
	gpmon_seginfo_t* rec;

	rec = apr_hash_get(agg->stab, &seg->dbid, sizeof(seg->dbid));
	if (rec) 
	{
		*rec = *seg;
	}
	else 
	{
		rec = apr_palloc(agg->pool, sizeof(*rec));
		if (!rec)	
		{
			return APR_ENOMEM;
		}
		*rec = *seg;
		apr_hash_set(agg->stab, &rec->dbid, sizeof(rec->dbid), rec);
    }
    return 0;
}

static apr_status_t agg_put_filerep(agg_t* agg, const gpmon_filerepinfo_t* filerep)
{
	gpmon_filrep_holder_t* rec;
	int addtoHash = 0;

	rec = apr_hash_get(agg->ftab, &filerep->key.dkey, sizeof(filerep->key.dkey));
	if (!rec) 
	{
		// use pcalloc to initialize to 0
		rec = apr_pcalloc(agg->pool, sizeof(*rec));
		if (!rec)	
		{
			return APR_ENOMEM;
		}
		memcpy(&rec->key, &filerep->key.dkey, (sizeof(rec->key)));
		addtoHash = 1;
	}

	if (filerep->key.isPrimary)
	{
		rec->primary = filerep->stats.primary;
		rec->elapsedTime_secs_primary = filerep->elapsedTime_secs;
	}
	else
	{
		rec->mirror = filerep->stats.mirror;
		rec->elapsedTime_secs_mirror = filerep->elapsedTime_secs;
	}	

	if (addtoHash)
	{
		apr_hash_set(agg->ftab, &rec->key, sizeof(rec->key), rec);
	}

    return 0;
}


static apr_status_t agg_put_query_metrics(agg_t* agg, const gpmon_qlog_t* qlog, apr_int64_t generation)
{
	qdnode_t* node;

	node = apr_hash_get(agg->qtab, &qlog->key, sizeof(qlog->key));
	if (node) 
	{
		// here update the stats for the query
		node->qlog.cpu_elapsed += qlog->cpu_elapsed;
		node->qlog.p_metrics.cpu_pct += qlog->p_metrics.cpu_pct;
		node->last_updated_generation = generation;
		node->num_metrics_packets++;
		TR2(("Query Metrics: (host %s ssid %d ccnt %d) (cpuelapsed %d cpupct %f) / %d\n", 
			qlog->user, qlog->key.ssid, qlog->key.ccnt, node->qlog.cpu_elapsed, node->qlog.p_metrics.cpu_pct,
			node->num_metrics_packets));
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
	node->qlog.cpu_elapsed = 0;
	node->qlog.p_metrics.cpu_pct = 0.0;
	node->num_metrics_packets = 0;
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
	qenode_t* ep;
	qenode_t** epp;
	int diff;

	/* find qdnode of this qexec */
	key.tmid = qexec->key.tmid;
	key.ssid = qexec->key.ssid;
	key.ccnt = qexec->key.ccnt;
	dp = apr_hash_get(agg->qtab, &key, sizeof(key));

	//TR0(("%s %s.%d.%d: %ld\n", (dp? "YES":"NO"), qexec->_hname, qexec->key.pid, qexec->key.nid, qexec->_cpu_elapsed));
	if (!dp) 
	{
    		/* not found, internal SPI query.  Ignore. */
    		return 0;
	}


	/* find qexec in qenode_list */
	diff = 0;
	for (ep = dp->qenode_list, epp = &dp->qenode_list; ep; epp = &ep->next, ep = ep->next) 
	{
		// different segment id, not a match
		diff = ep->qexec.key.segid - qexec->key.segid;
		if (diff)
			continue;

		// different node id, not a match
		diff = ep->qexec.key.nid - qexec->key.nid;
		if (diff)
	    		continue;

		// complete match break
		diff = ep->qexec.key.pid - qexec->key.pid;
		if (!diff)
	    		break;
    	}

	/* if found, replace it */
	if (ep && diff == 0) 
	{
		ep->qexec = *qexec;
		ep->cpu_elapsed = qexec->_cpu_elapsed;
	}
	else 
	{
		qenode_t* x;
		/* not found, make new qenode */
		if (! (x = apr_palloc(agg->pool, sizeof(*x))))
	    	return APR_ENOMEM;

		x->qexec = *qexec;
		x->cpu_elapsed = qexec->_cpu_elapsed;
		x->next = ep;
		*epp = x;
	}

	if (0) 
	{
		int cnt = 0;
		TR0(("list now:\n"));

		for (ep = dp->qenode_list; ep; ep = ep->next) 
		{
		TR0(("%d: %s.%d.%d: %ld\n", ++cnt, ep->qexec._hname, ep->qexec.key.nid, ep->qexec.key.pid, ep->cpu_elapsed));
		}
		TR0(("  -- \n"));
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

    agg->stab = apr_hash_make(pool);
    if (!agg->stab) {
	apr_pool_destroy(pool);
	return APR_ENOMEM;
    }

    agg->ftab = apr_hash_make(pool);
    if (!agg->ftab) {
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
	apr_int32_t status;

	apr_hash_this(hi, 0, 0, &vptr);
	dp = vptr;

	/* skip all entries that weren't updated recently and aren't waiting in a queue */
	/* Read status from query text as this is reliable */
	status = get_query_status(dp->qlog.key.tmid, dp->qlog.key.ssid, dp->qlog.key.ccnt);
	if (dp->last_updated_generation < newagg->generation - 1
            && status != GPMON_QLOG_STATUS_SUBMIT
            && status != GPMON_QLOG_STATUS_START) {
	    if (0 != strcmp(dp->qlog.db, GPMON_DB)) {
		TR2(("agg_dup: skip %d.%d.%d generation %d, current generation %d, recorded %d\n",
		    dp->qlog.key.tmid, dp->qlog.key.ssid, dp->qlog.key.ccnt,
		    (int) dp->last_updated_generation,
		    (int) newagg->generation, dp->recorded));
	    }
	    continue;
	}

	/* check if we missed a status change */
	if (dp->qlog.status != status)
		dp->qlog.status = status;

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

	// reset metrics that are accumulated each quantum
	newdp->qlog.cpu_elapsed = 0;
	newdp->qlog.p_metrics.cpu_pct = 0.0;
	newdp->num_metrics_packets = 0;

	apr_hash_set(newagg->qtab, &newdp->qlog.key, sizeof(newdp->qlog.key), newdp);

	/* dup all qexec entries hanging off dp */
	cnt = 0;
	tail = &newdp->qenode_list;
	for (ep = dp->qenode_list; ep; ep = ep->next) {
	    qenode_t* x;
	    if (! (x = apr_pcalloc(newagg->pool, sizeof(*x)))) {
		agg_destroy(newagg);
		return APR_ENOMEM;
	    }
	    x->qexec = ep->qexec;
		x->cpu_elapsed = ep->cpu_elapsed;
	    *tail = x;
	    tail = &x->next;

	    TR2(("\t    %d: (%d, %d) op %d\n", ++cnt,
		 x->qexec.key.segid,
		 x->qexec.key.nid,
		 x->qexec.nodeType));
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
	if (pkt->pkttype == GPMON_PKTTYPE_SEGINFO)
		return agg_put_segment(agg, &pkt->u.seginfo);
	if (pkt->pkttype == GPMON_PKTTYPE_FILEREP)
		return agg_put_filerep(agg, &pkt->u.filerepinfo);
	if (pkt->pkttype == GPMON_PKTTYPE_QUERY_HOST_METRICS)
		return agg_put_query_metrics(agg, &pkt->u.qlog, agg->generation);

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

static apr_uint32_t write_system(agg_t* agg, const char* nowstr);
static apr_uint32_t write_segmentinfo(agg_t* agg, char* nowstr);
static apr_uint32_t write_filerepinfo(agg_t* agg, char* nowstr);
static apr_uint32_t write_dbmetrics(dbmetrics_t* dbmetrics, char* nowstr);
static apr_uint32_t write_qlog(FILE* fp, qdnode_t *qdnode, const char* nowstr, apr_uint32_t done);
static apr_uint32_t write_qlog_full(FILE* fp, qdnode_t *qdnode, const char* nowstr);

#define SINGLE_METRIC_BUFSZ 100
typedef char single_metric_string[SINGLE_METRIC_BUFSZ];

/* write the iterator table row to disk.
   note the for tail / history tables we dont record current cpu % but we do for now tables
*/
static apr_uint32_t write_qenode_row(FILE* fp, const qenode_t* qenode, const char* nowstr, bool historicalinfo)
{
	const int WRITE_QENODE_ROW_BUF_SIZE = 2048;
	char line[WRITE_QENODE_ROW_BUF_SIZE];
	single_metric_string metrics[GPMON_QEXEC_M_DBCOUNT];
	int bytes_written = 0;
	char qexec_tstamp[GPMON_DATE_BUF_SIZE];
	int i;
	const gpmon_qexec_t* qexec = &qenode->qexec;
	const char* measure_name = NULL;
	const char* unit_name = NULL;
	apr_status_t r = APR_SUCCESS;
	char phase[4] = " ";
	char tname[16] = "Name";

	for (i = 0; i < GPMON_QEXEC_M_DBCOUNT; i++) 
	{
		if (r == APR_SUCCESS)
		{
			/* after this function returns failure 1 time don't try again for remaing numbers in array */

			/* GPMON_QEXEC_M_DBCOUNT is number of metrics output to the DB
				GPMON_QEXEC_M_COUNT   is number of metrics in the packets */

			if (i < GPMON_QEXEC_M_COUNT)
			{
				r = gpdb_getnode_metricinfo(qexec->nodeType, i, &measure_name, &unit_name);
			}
			else
			{
				r = APR_NOTFOUND;
			}
		}

		if (r == APR_SUCCESS)
		{
			snprintf(metrics[i], SINGLE_METRIC_BUFSZ, "%s|%s|%" FMT64 "|0", measure_name, unit_name, qexec->measures[i]);
		}
		else
		{
			snprintf(metrics[i], SINGLE_METRIC_BUFSZ, "%s", "null|null|null|null");
		}
	}

	snprintf(line, WRITE_QENODE_ROW_BUF_SIZE, 
		"%s|%d|%d|%d|%d|%d|%d|%d|%s|%s|%s|%s|%d|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%.2f|%s|%" FMT64 "|%" FMT64 "|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s" "|%s|%s",
		nowstr,
		qexec->key.tmid,
		qexec->key.ssid,
		qexec->key.ccnt,
		qexec->key.segid,
		qexec->key.pid,
		qexec->key.nid,
		qexec->pnid,
		qexec->_hname,
		gpdb_getnodename(qexec->nodeType),
		gpdb_getnodestatus(qexec->status),
		gpmon_datetime((time_t)qexec->tstart, qexec_tstamp),
		qexec->tduration,
		qexec->p_mem, 
		qexec->p_memmax,
		qexec->_p_metrics.mem.size,
		qexec->_p_metrics.mem.resident,
		qexec->_p_metrics.mem.share,
		qenode->cpu_elapsed,
		(historicalinfo ? 0.0 : qexec->_p_metrics.cpu_pct),
		phase,
		qexec->rowsout, 
		qexec->rowsout_est,
		metrics[0], metrics[1], metrics[2], metrics[3],
		metrics[4], metrics[5], metrics[6], metrics[7],
		metrics[8], metrics[9], metrics[10], metrics[11],
		metrics[12], metrics[13], metrics[14], metrics[15],
		tname,
		qexec->relation_name);

	bytes_written = strlen(line) + 1;
	if (bytes_written == WRITE_QENODE_ROW_BUF_SIZE)
	{
		gpmon_warning(FLINE, "iterator line too long ... ignored: %s", line);
		return 0;
	}

	fprintf(fp, "%s\n", line);
	return bytes_written;
}



apr_status_t agg_dump(agg_t* agg)
{
	apr_hash_index_t* hi;
	bloom_t bloom;
	char nowstr[GPMON_DATE_BUF_SIZE];
	int e = 0;
	FILE* fp_iters_now = 0;
	FILE* fp_iters_tail = 0;
	FILE* fp_queries_now = 0;
	FILE* fp_queries_tail = 0;

	dbmetrics_t dbmetrics = {0};

	apr_uint32_t temp_bytes_written = 0;

	gpmon_datetime_rounded(time(NULL), nowstr);

	bloom_init(&bloom);

	/* we never delete system_tail/ system_now/
		queries_tail/ queries_now/
		iterators_tail/ iterators_now/ files */
	bloom_set(&bloom, GPMON_DIR "system_now.dat");
	bloom_set(&bloom, GPMON_DIR "system_tail.dat");
	bloom_set(&bloom, GPMON_DIR "system_stage.dat");
	bloom_set(&bloom, GPMON_DIR "_system_tail.dat");
	bloom_set(&bloom, GPMON_DIR "queries_now.dat");
	bloom_set(&bloom, GPMON_DIR "queries_tail.dat");
	bloom_set(&bloom, GPMON_DIR "queries_stage.dat");
	bloom_set(&bloom, GPMON_DIR "_queries_tail.dat");
	bloom_set(&bloom, GPMON_DIR "iterators_now.dat");
	bloom_set(&bloom, GPMON_DIR "iterators_tail.dat");
	bloom_set(&bloom, GPMON_DIR "iterators_stage.dat");
	bloom_set(&bloom, GPMON_DIR "_iterators_tail.dat");
	bloom_set(&bloom, GPMON_DIR "database_now.dat");
	bloom_set(&bloom, GPMON_DIR "database_tail.dat");
	bloom_set(&bloom, GPMON_DIR "database_stage.dat");
	bloom_set(&bloom, GPMON_DIR "_database_tail.dat");
	bloom_set(&bloom, GPMON_DIR "segment_now.dat");
	bloom_set(&bloom, GPMON_DIR "segment_tail.dat");
	bloom_set(&bloom, GPMON_DIR "segment_stage.dat");
	bloom_set(&bloom, GPMON_DIR "_segment_tail.dat");
	bloom_set(&bloom, GPMON_DIR "filerep_now.dat");
	bloom_set(&bloom, GPMON_DIR "filerep_tail.dat");
	bloom_set(&bloom, GPMON_DIR "filerep_stage.dat");
	bloom_set(&bloom, GPMON_DIR "_filerep_tail.dat");
	

	/* dump metrics */
	temp_bytes_written = write_system(agg, nowstr);
	incremement_tail_bytes(temp_bytes_written);

	/* write segment metrics */
	temp_bytes_written = write_segmentinfo(agg, nowstr);
	incremement_tail_bytes(temp_bytes_written);

	/* write filerep metrics */
	temp_bytes_written = write_filerepinfo(agg, nowstr);
	incremement_tail_bytes(temp_bytes_written);

	if (! (fp_queries_tail = fopen(GPMON_DIR "queries_tail.dat", "a"))) 
	{
		e = APR_FROM_OS_ERROR(errno);
		goto bail;
	}

	/* loop through queries */
	for (hi = apr_hash_first(0, agg->qtab); hi; hi = apr_hash_next(hi)) 
	{
		void* vptr;
		qdnode_t* qdnode;
		apr_hash_this(hi, 0, 0, &vptr);
		qdnode = vptr;
	
		if (qdnode->qlog.status == GPMON_QLOG_STATUS_DONE || qdnode->qlog.status == GPMON_QLOG_STATUS_ERROR) 
		{
		    if (!qdnode->recorded && ((qdnode->qlog.tfin - qdnode->qlog.tstart) >= min_query_time)) 
			{
				TR1(("queries_tail: %x add query %d.%d.%d, status %d, generation %d, recorded %d\n",
			    	agg->qtab, qdnode->qlog.key.tmid, qdnode->qlog.key.ssid, qdnode->qlog.key.ccnt, qdnode->qlog.status, qdnode->last_updated_generation, qdnode->recorded));
	
				temp_bytes_written += write_qlog_full(fp_queries_tail, qdnode, nowstr);
				incremement_tail_bytes(temp_bytes_written);

				qdnode->recorded = 1;
		    }
		}
		else
		{
			switch (qdnode->qlog.status)
			{
			case GPMON_QLOG_STATUS_START:
				dbmetrics.queries_running++;
				break;
			case GPMON_QLOG_STATUS_SUBMIT:
	                	dbmetrics.queries_queued++;
	                	break;
			default:
	                	/* Not interested */
				break;
			}
		}
	}
	dbmetrics.queries_total = dbmetrics.queries_running + dbmetrics.queries_queued;

	fclose(fp_queries_tail);
	fp_queries_tail = 0;

	/* dump dbmetrics */
	temp_bytes_written += write_dbmetrics(&dbmetrics, nowstr);
	incremement_tail_bytes(temp_bytes_written);

	if (! (fp_queries_now = fopen(GPMON_DIR "_queries_now.dat", "w"))) 
	{
		e = APR_FROM_OS_ERROR(errno);
		goto bail;
	}

	if (! (fp_iters_now = fopen(GPMON_DIR "_iterators_now.dat", "w"))) 
	{
		e = APR_FROM_OS_ERROR(errno);
		goto bail;
	}

	if (! (fp_iters_tail = fopen(GPMON_DIR "iterators_tail.dat", "a"))) 
	{
		e = APR_FROM_OS_ERROR(errno);
		goto bail;
	}

	for (hi = apr_hash_first(0, agg->qtab); hi; hi = apr_hash_next(hi)) 
	{
		void* vptr;
		qdnode_t* qdnode;
		qenode_t* ep;

		apr_hash_this(hi, 0, 0, &vptr);
		qdnode = vptr;

		/* don't touch this file */
		{
			const int fname_size = sizeof(GPMON_DIR) + 100;
			char fname[fname_size];
			snprintf(fname, fname_size, GPMON_DIR "q%d-%d-%d.txt",
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
		if (qdnode->qlog.status != GPMON_QLOG_STATUS_DONE && qdnode->qlog.status != GPMON_QLOG_STATUS_ERROR)
		{
			write_qlog(fp_queries_now, qdnode, nowstr, 0);
		}
		else if (qdnode->qlog.tfin - qdnode->qlog.tstart >= min_query_time)
		{
			write_qlog(fp_queries_now, qdnode, nowstr, 1);
		}

		/* write iterators now data */
		for (ep = qdnode->qenode_list; ep; ep = ep->next) 
		{
			write_qenode_row(fp_iters_now, ep, nowstr, false);
		}

		/* write iterators tail data */
		if (qdnode->recorded && !qdnode->qenode_recorded && ((qdnode->qlog.tfin - qdnode->qlog.tstart) >= min_detailed_query_time)) 
		{
		    for (ep = qdnode->qenode_list; ep; ep = ep->next) 
			{
				temp_bytes_written += write_qenode_row(fp_iters_tail, ep, nowstr, true);
				incremement_tail_bytes(temp_bytes_written);
			}
			qdnode->qenode_recorded = 1;
		}
	}

	if (fp_queries_now) fclose(fp_queries_now);
	if (fp_iters_now) fclose(fp_iters_now);
	if (fp_iters_tail) fclose(fp_iters_tail);
	if (fp_queries_tail) fclose(fp_queries_tail);
	rename(GPMON_DIR "_system_now.dat", GPMON_DIR "system_now.dat");
	rename(GPMON_DIR "_segment_now.dat", GPMON_DIR "segment_now.dat");
	rename(GPMON_DIR "_iterators_now.dat", GPMON_DIR "iterators_now.dat");
	rename(GPMON_DIR "_queries_now.dat", GPMON_DIR "queries_now.dat");
	rename(GPMON_DIR "_database_now.dat", GPMON_DIR "database_now.dat");
	rename(GPMON_DIR "_filerep_now.dat", GPMON_DIR "filerep_now.dat");
	
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
	char findDir[256] = {0};
	char findCmd[512] = {0};
	FILE* fp = NULL;
    time_t cutoff = time(0) - gpmmon_quantum() * 3;

    /* Need to remove trailing / in dir so find results are consistent
     * between platforms
     */
    strncpy(findDir, GPMON_DIR, 255);
    if (findDir[strlen(findDir) -1] == '/')
    	findDir[strlen(findDir) - 1] = '\0';

    snprintf(findCmd, 512, "find %s -name \"q*-*.txt\" 2> /dev/null", findDir);
    fp = popen(findCmd, "r");

    if (fp)
	{
		for (;;)
		{
		    char line[1024];
		    char* p;
		    struct stat stbuf;
			apr_int32_t status;

		    line[sizeof(line) - 1] = 0;
		    if (! (p = fgets(line, sizeof(line), fp)))
				break;
		    if (line[sizeof(line) - 1])
				continue; 	/* fname too long */

		    p = gpmon_trim(p);
		    TR2(("Checking file %s\n", p));

		    if (0 == stat(p, &stbuf))
			{
#if defined(sun) || defined(linux)
				int expired = stbuf.st_mtime < cutoff;
#else
				int expired = stbuf.st_mtimespec.tv_sec < cutoff;
#endif
				TR2(("File %s expired: %d\n", p, expired));
				if (expired)
				{
					apr_int32_t tmid = 0, ssid = 0, ccnt = 0;
					if (bloom_isset(bloom, p))
					{
						TR2(("File %s has bloom set.  Checking status\n", p));
						/* Verify no bloom collision */
						sscanf(p, GPMON_DIR "q%d-%d-%d.txt", &tmid, &ssid, &ccnt);
						TR2(("tmid: %d, ssid: %d, ccnt: %d\n", tmid, ssid, ccnt));
						status = get_query_status(tmid, ssid, ccnt);
						TR2(("File %s has status of %d\n", p, status));
						if (status == GPMON_QLOG_STATUS_DONE ||
						   status == GPMON_QLOG_STATUS_ERROR)
						{
							TR2(("Deleting file %s\n", p));
							unlink(p);
						}
					}
					else
					{
						TR2(("Deleting file %s\n", p));
						unlink(p);
					}
				}
		    }
		}
		pclose(fp);
    }
    else
    {
    	gpmon_warning(FLINE, "Failed to get a list of query text files.\n");
    }
}

static apr_uint32_t write_filerepinfo(agg_t* agg, char* nowstr)
{
	const int million = 1000000;
	apr_uint32_t bytes_written = 0;
	FILE* fp = fopen(GPMON_DIR "filerep_tail.dat", "a");
	FILE* fp2 = fopen(GPMON_DIR "_filerep_now.dat", "w");
	apr_hash_index_t* hi;
	const int line_size = 2048;
	char line[line_size];

	if (!fp || !fp2) 
	{
		if (fp) fclose(fp);
		if (fp2) fclose(fp2);
		return 0;
	}

	for (hi = apr_hash_first(0, agg->ftab); hi; hi = apr_hash_next(hi)) 
	{
		gpmon_filrep_holder_t* sp;
		int bytes_this_record;
		void* valptr = 0;
		apr_hash_this(hi, 0, 0, (void**) &valptr);
		sp = (gpmon_filrep_holder_t*) valptr;

		snprintf(line, line_size, "%s|%u|%u|%s|%d|%s|%d|" // through mirror_port
					  "%d|%d|%d|%d|%.2f|" // primary write syscall
					  "%d|%d|%.2f|" // primary fsync syscall
					  "%d|%d|%d|%d|%.2f|" // primary write shmem
					  "%d|%d|%.2f|" // primary fsync shmem
					  "%d|%d|%.2f|" // primary fsync roundtrip
					  "%d|%d|%.2f|" // primary test roundtrip
					  "%d|%d|%d|%d|%.2f|" // mirror write syscall
					  "%d|%d|%.2f", // mirror fsync syscall
			nowstr, 
			(apr_uint32_t)(sp->elapsedTime_secs_primary * million),
			(apr_uint32_t)(sp->elapsedTime_secs_primary * million),
			sp->key.primary_hostname,
			sp->key.primary_port,
			sp->key.mirror_hostname,
			sp->key.mirror_port,
			sp->primary.write_syscall_size_avg,
			sp->primary.write_syscall_size_max,
			sp->primary.write_syscall_time_avg,
			sp->primary.write_syscall_time_max,
			sp->primary.write_syscall_count / sp->elapsedTime_secs_primary,
			sp->primary.fsync_syscall_time_avg,
			sp->primary.fsync_syscall_time_max,
			sp->primary.fsync_syscall_count  / sp->elapsedTime_secs_primary,
			sp->primary.write_shmem_size_avg,
			sp->primary.write_shmem_size_max,
			sp->primary.write_shmem_time_avg,
			sp->primary.write_shmem_time_max,
			sp->primary.write_shmem_count / sp->elapsedTime_secs_primary,
			sp->primary.fsync_shmem_time_avg,
			sp->primary.fsync_shmem_time_max,
			sp->primary.fsync_shmem_count / sp->elapsedTime_secs_primary,
			sp->primary.roundtrip_fsync_msg_time_avg,
			sp->primary.roundtrip_fsync_msg_time_max,
			sp->primary.roundtrip_fsync_msg_count / sp->elapsedTime_secs_primary,
			sp->primary.roundtrip_test_msg_time_avg, 
			sp->primary.roundtrip_test_msg_time_max,
			sp->primary.roundtrip_test_msg_count / sp->elapsedTime_secs_primary,
			sp->mirror.write_syscall_size_avg,
			sp->mirror.write_syscall_size_max,
			sp->mirror.write_syscall_time_avg,
			sp->mirror.write_syscall_time_max, 
			sp->mirror.write_syscall_count / sp->elapsedTime_secs_mirror, 
			sp->mirror.fsync_syscall_time_avg, 
			sp->mirror.fsync_syscall_time_max,
			sp->mirror.fsync_syscall_count / sp->elapsedTime_secs_mirror);

		bytes_this_record = strlen(line) + 1;
		if (bytes_this_record == line_size)
		{
			gpmon_warning(FLINE, "filerep stats line to too long ... ignored: %s", line);
			continue;
		}
		fprintf(fp, "%s\n", line);
		fprintf(fp2, "%s\n", line);
		bytes_written += bytes_this_record;
    }

	fclose(fp);
	fclose(fp2);

	return bytes_written;
}

static apr_uint32_t write_segmentinfo(agg_t* agg, char* nowstr)
{
	FILE* fp = fopen(GPMON_DIR "segment_tail.dat", "a");
	FILE* fp2 = fopen(GPMON_DIR "_segment_now.dat", "w");
	apr_hash_index_t* hi;
	const int line_size = 256;
	char line[line_size];
	apr_uint32_t bytes_written = 0;

	if (!fp || !fp2) 
	{
		if (fp) fclose(fp);
		if (fp2) fclose(fp2);
		return 0;
	}

	for (hi = apr_hash_first(0, agg->stab); hi; hi = apr_hash_next(hi)) 
	{
		gpmon_seginfo_t* sp;
		int bytes_this_record;
		void* valptr = 0;
		apr_hash_this(hi, 0, 0, (void**) &valptr);
		sp = (gpmon_seginfo_t*) valptr;

		snprintf(line, line_size, "%s|%d|%s|%" FMTU64 "|%" FMTU64, nowstr, sp->dbid, sp->hostname, sp->dynamic_memory_used, sp->dynamic_memory_available);

		bytes_this_record = strlen(line) + 1;
		if (bytes_this_record == line_size)
		{
			gpmon_warning(FLINE, "segmentinfo line to too long ... ignored: %s", line);
			continue;
		}
		fprintf(fp, "%s\n", line);
		fprintf(fp2, "%s\n", line);
		bytes_written += bytes_this_record;
    }

	fclose(fp);
	fclose(fp2);
	return bytes_written;
}

static apr_uint32_t write_dbmetrics(dbmetrics_t* dbmetrics, char* nowstr)
{
	FILE* fp = fopen(GPMON_DIR "database_tail.dat", "a");
	FILE* fp2 = fopen(GPMON_DIR "_database_now.dat", "w");
	int e;
	const int line_size = 256;
	char line[line_size];
	int bytes_written;

	if (!fp || !fp2)
	{
		e = APR_FROM_OS_ERROR(errno);
		if (fp) fclose(fp);
		if (fp2) fclose(fp2);
		return e;
	}

	snprintf(line, line_size, "%s|%d|%d|%d", nowstr,
             dbmetrics->queries_total,
             dbmetrics->queries_running,
             dbmetrics->queries_queued);

	bytes_written = strlen(line) + 1;
	if (bytes_written == line_size){
		gpmon_warning(FLINE, "dbmetrics line too long ... ignored:: %s", line);
		return 0;
	}

	fprintf(fp, "%s\n", line);
    fprintf(fp2, "%s\n", line);

    fclose(fp);
    fclose(fp2);

    return bytes_written;
}

static apr_uint32_t write_system(agg_t* agg, const char* nowstr)
{
	FILE* fp = fopen(GPMON_DIR "system_tail.dat", "a");
	FILE* fp2 = fopen(GPMON_DIR "_system_now.dat", "w");
	apr_hash_index_t* hi;
	const int line_size = 1000;
	char line[line_size];
	apr_uint32_t bytes_written = 0;

	if (!fp || !fp2) 
	{
		if (fp) fclose(fp);
		if (fp2) fclose(fp2);
		return 0;
 	}

	for (hi = apr_hash_first(0, agg->htab); hi; hi = apr_hash_next(hi)) 
	{
		gpmon_metrics_t* mp;
		void* valptr = 0;
		int quantum = gpmmon_quantum();
		int bytes_this_line;
		apr_hash_this(hi, 0, 0, (void**) &valptr);
		mp = (gpmon_metrics_t*) valptr;

		snprintf(line, line_size, 
		"%s|%s|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%.2f|%.2f|%.2f|%.4f|%.4f|%.4f|%d|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64 "|%" FMT64,
		nowstr, 
		mp->hname,
		mp->mem.total, 
		mp->mem.used, 
		mp->mem.actual_used, 
		mp->mem.actual_free,
		mp->swap.total, 
		mp->swap.used,
		(apr_int64_t)ceil((double)mp->swap.page_in / (double)quantum),
		(apr_int64_t)ceil((double)mp->swap.page_out / (double)quantum),
		mp->cpu.user_pct, 
		mp->cpu.sys_pct, 
		mp->cpu.idle_pct,
		mp->load_avg.value[0], 
		mp->load_avg.value[1], 
		mp->load_avg.value[2],
		quantum,
		mp->disk.ro_rate,
		mp->disk.wo_rate,
		mp->disk.rb_rate,
		mp->disk.wb_rate,
		mp->net.rp_rate,
		mp->net.wp_rate,
		mp->net.rb_rate,
		mp->net.wb_rate);
		
		bytes_this_line = strlen(line) + 1;
		if (bytes_this_line == line_size){
			gpmon_warning(FLINE, "system metrics line too long ... ignored: %s", line);
			continue;
		}

		fprintf(fp, "%s\n", line);
		fprintf(fp2, "%s\n", line);

		bytes_written += bytes_this_line;
	}

	fclose(fp);
	fclose(fp2);
	return bytes_written;
}

static apr_int64_t get_rowsout(qdnode_t* qdnode)
{
    qenode_t* pqe = NULL;
    apr_int64_t rowsout = 0;

    for (pqe = qdnode->qenode_list; pqe; pqe = pqe->next)
        if (pqe->qexec.key.segid == -1 && pqe->qexec.key.nid == 1)
        {
            rowsout = pqe->qexec.rowsout;
            break;
        }

    return rowsout;
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
	{
        gpmon_warningx(FLINE, e, "apr_pool_create failed");
        return 0.0f;
	}

    segtab = apr_hash_make(tmp_pool);
    if (!segtab)
	{
        gpmon_warning(FLINE, "Out of memory");
        return 0.0f;
	}

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

    apr_int64_t row_out_avg = 0;
    apr_int64_t* seg_row_out_sum = NULL;
    void* valptr;

    double var = 0.0f;

    int segcnt = 0;
    int e;

    if (!qdnode)
        return 0.0f;

    if (0 != (e = apr_pool_create(&tmp_pool, 0)))
	{
        gpmon_warningx(FLINE, e, "apr_pool_create failed");
        return 0.0f;
	}

    segtab = apr_hash_make(tmp_pool);
    if (!segtab)
	{
        gpmon_warning(FLINE, "Out of memory");
        return 0.0f;
	}

    /* Calc rows in sum per segment */
    for (pqe = qdnode->qenode_list; pqe; pqe = pqe->next)
    {
        /* Skip QD */
        if (pqe->qexec.key.segid == -1)
            continue;

        seg_row_out_sum = apr_hash_get(segtab, &pqe->qexec.key.segid,
                                   sizeof(pqe->qexec.key.segid));

        if (!seg_row_out_sum) {
            seg_row_out_sum = apr_palloc(tmp_pool, sizeof(apr_int64_t));
            *seg_row_out_sum = 0;
        }

        *seg_row_out_sum += pqe->qexec.measures[GPMON_QEXEC_M_ROWSIN];
        TR2(("(SKEW) Iterator data for seg %d.  Rows out: %" FMT64 "\n",
             pqe->qexec.key.segid, pqe->qexec.rowsout));
        apr_hash_set(segtab, &pqe->qexec.key.segid,
                     sizeof(pqe->qexec.key.segid), seg_row_out_sum);
    }

    /* Calc rows in mean across all segments */
    for (hi = apr_hash_first(NULL, segtab); hi; hi = apr_hash_next(hi))
    {
        apr_hash_this(hi, 0, 0, &valptr);
	seg_row_out_sum = (apr_int64_t*) valptr;
        row_out_avg += *seg_row_out_sum;
        segcnt++;
    }

    if (!segcnt) {
        TR2(("No segments for CPU skew calculation\n"));
        apr_pool_destroy(tmp_pool);
        return 0.0f;
    }

    row_out_avg = row_out_avg / segcnt;

    TR2(("(SKEW) Avg rows out: %" FMT64 "\n", row_out_avg));

    /* Calc sqrt of dev squared mean */
    for (hi = apr_hash_first(NULL, segtab); hi; hi = apr_hash_next(hi))
    {
        apr_int64_t dev = 0;

        apr_hash_this(hi, NULL, NULL, &valptr);
	seg_row_out_sum = (apr_int64_t*) valptr;
        dev = *seg_row_out_sum - row_out_avg;
        TR2(("(SKEW) Deviation: %d\n", dev));
        var += dev * dev;
    }

    var = sqrt(var / (double)segcnt);

    TR2(("(SKEW) Rows in variance: %f\n", var));

    /* Skew calc */
    apr_pool_destroy(tmp_pool);
    TR2(("(SKEW) Rows out skew: %f\n", row_out_avg ? var/(double)row_out_avg : 0.0f));

    return (row_out_avg ? (var/(double)row_out_avg) * 100.0f : 0.0f);
}


static void fmt_qlog(char* line, const int line_size, qdnode_t* qdnode, const char* nowstr, apr_uint32_t done)
{
	char timsubmitted[GPMON_DATE_BUF_SIZE];
	char timstarted[GPMON_DATE_BUF_SIZE];
	char timfinished[GPMON_DATE_BUF_SIZE];
	double cpu_skew = 0.0f;
	double row_skew = 0.0f;
	int query_hash = 0;
	apr_int64_t rowsout = 0;
	float cpu_current;
	cpu_skew = get_cpu_skew(qdnode);
	row_skew = get_row_skew(qdnode);
	rowsout = get_rowsout(qdnode);
	gpmon_datetime((time_t)qdnode->qlog.tsubmit, timsubmitted);

	if (qdnode->qlog.tstart)
	{
		gpmon_datetime((time_t)qdnode->qlog.tstart, timstarted); 
	}
	else
	{
		snprintf(timstarted, GPMON_DATE_BUF_SIZE, "null");
	}

	if (done)
	{
		cpu_current = 0.0f;
		gpmon_datetime((time_t)qdnode->qlog.tfin, timfinished);
	}
	else
	{
		if (qdnode->num_metrics_packets)
		{
			// average cpu_pct per reporting machine
			cpu_current = qdnode->qlog.p_metrics.cpu_pct / qdnode->num_metrics_packets;
		}
		else
		{
			cpu_current = 0.0f;
		}
		snprintf(timfinished, GPMON_DATE_BUF_SIZE,  "null");
	}

	snprintf(line, line_size, "%s|%d|%d|%d|%s|%s|%d|%s|%s|%s|%s|%" FMT64 "|%" FMT64 "|%.4f|%.2f|%.2f|%d",
		nowstr,
		qdnode->qlog.key.tmid, 
		qdnode->qlog.key.ssid, 
		qdnode->qlog.key.ccnt,
		qdnode->qlog.user, 
		qdnode->qlog.db,
		qdnode->qlog.cost,
		timsubmitted,
		timstarted,
		timfinished,
		gpmon_qlog_status_string(qdnode->qlog.status),
		rowsout,
		qdnode->qlog.cpu_elapsed,
		cpu_current,
		cpu_skew, 
		row_skew,
		query_hash);
}


static apr_uint32_t write_qlog(FILE* fp, qdnode_t *qdnode, const char* nowstr, apr_uint32_t done)
{
	const int line_size = 1024;
	char line[line_size];
	int bytes_written;

	fmt_qlog(line, line_size, qdnode, nowstr, done);
	bytes_written = strlen(line) + 1;

	if (bytes_written == line_size)
	{
		gpmon_warning(FLINE, "qlog line too long ... ignored: %s", line);
		return 0;
	}
	else
	{
		/* Query text "joined" by python script */
		fprintf(fp, "%s|||||\n", line);
		return bytes_written;
	}
}

static int get_and_print_next_query_file_kvp(FILE* outfd, FILE* queryfd, char* qfname, apr_uint32_t* bytes_written)
{
    const int line_size = 1024;
    char line[line_size];
    line[0] = 0;
    char *p = NULL;
    int field_len = 0;
    int retCode = 0;

    p = fgets(line, line_size, queryfd);
    line[line_size-1] = 0; // in case libc is buggy

    if (!p) {
	    gpmon_warning(FLINE, "Error parsing file: %s", qfname);
        return APR_NOTFOUND;
    }

    retCode = sscanf(p, "%d", &field_len);

    if (1 != retCode){
	    gpmon_warning(FLINE, "bad format on file: %s", qfname);
        return APR_NOTFOUND;
    }

    if (field_len < 0) {
	    gpmon_warning(FLINE, "bad field length on file: %s", qfname);
        return APR_NOTFOUND;
	}

    if (!field_len) {
        // empty field, read through the newline
        p = fgets(line, line_size, queryfd);
        if (p)
            return APR_SUCCESS;
        else
            return APR_NOTFOUND;
    }

    fprintf(outfd, "\"");
    (*bytes_written)++;

    while (field_len > 0) {
	    int max, n;
        char* q;
        max = field_len > sizeof(line) ? sizeof(line) : field_len;
        n = fread(line, 1, max, queryfd);
        for (p = line, q = line + n; p < q; p++) 
        {
            if (*p == '"')
            {
                fputc('\"', outfd);
                (*bytes_written)++;
            }

            if ((*p == '\n') || (*p == '\r')) 
                fputc(' ', outfd);
            else 
                fputc(*p, outfd);

            (*bytes_written)++;

        }
        field_len -= n;
        if (n < max) break;
    }

	fprintf(outfd, "\"");
	(*bytes_written)++;

    int n = fread(line, 1, 1, queryfd);
    if (n != 1)
    {
	    gpmon_warning(FLINE, "missing expeceted newline in file: %s", qfname);
        return APR_NOTFOUND;
    }

    return APR_SUCCESS;
}

static apr_uint32_t write_qlog_full(FILE* fp, qdnode_t *qdnode, const char* nowstr)
{
	const int line_size = 1024;
	const int qfname_size = 256;
    char line[line_size];
    char qfname[qfname_size];
    FILE* qfptr = 0;
    apr_uint32_t bytes_written = 0;

    fmt_qlog(line, line_size, qdnode, nowstr, 1);
    bytes_written = strlen(line) + 1;
	if (bytes_written == line_size)
	{
		gpmon_warning(FLINE, "qlog line too long ... ignored: %s", line);
		return 0;
	}

	fprintf(fp, "%s", line);

	snprintf(qfname, qfname_size, GPMON_DIR "q%d-%d-%d.txt", qdnode->qlog.key.tmid,
            qdnode->qlog.key.ssid, qdnode->qlog.key.ccnt);

	qfptr = fopen(qfname, "r");
    if (!qfptr)
    {
	    fprintf(fp, "|||||\n");
	    bytes_written += 6;
	    return bytes_written;
    }

    // 0 add query text
    // 1 add query plan
    // 2 add application name
    // 3 add rsqname 
    // 4 add priority

    int total_iterations = 5;
    int all_good = 1;
    int iter;
    int retCode = APR_SUCCESS;
    for (iter = 0; iter < total_iterations; ++iter)
    {
	    fprintf(fp, "|");
        bytes_written++;

        if (!all_good || iter == 1){
            // we have no data for query plan
            // if we failed once already don't bother trying to parse query file
            continue;
        }

        retCode = get_and_print_next_query_file_kvp(fp, qfptr, qfname, &bytes_written);
        if (retCode != APR_SUCCESS)
            all_good = 0;
    }

    fprintf(fp, "\n");
    fclose(qfptr);
	return bytes_written;
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


