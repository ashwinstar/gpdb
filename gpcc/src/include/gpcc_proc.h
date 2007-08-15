#ifndef GPCC_PROC_H
#define GPCC_PROC_H

#include <apr_thread_proc.h>
#include <apr_pools.h>

APR_DECLARE(apr_status_t) gpcc_proc_run_shell(apr_proc_t *proc, const char *prog, const char *const *argv, const char * const *env, int wait, apr_pool_t *p);

APR_DECLARE(apr_status_t) gpcc_proc_run_prog(apr_proc_t *proc, const char *prog, const char *const *argv, const char *const *env, int wait, apr_pool_t *p);

APR_DECLARE(apr_status_t) gpcc_proc_psql(apr_proc_t *proc, const char *sql, int wait, apr_pool_t *p);

APR_DECLARE(apr_status_t) gpcc_proc_ssh(apr_proc_t *proc, const char *cmd, int wait, apr_pool_t *p);

#endif
