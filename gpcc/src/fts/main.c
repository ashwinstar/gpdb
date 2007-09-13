#include <stdio.h>
#include <stdlib.h>
#include <apr.h>
#include <gpcc.h>
#include <apr_queue.h>
#include "fts.h"

int main(int argc, const char *argv[]) 
{
    apr_pool_t *p;
    apr_queue_t *alertq;
    apr_queue_t *outq;
    char *out;
    int e;

    apr_app_initialize(&argc, (char const *const **) &argv, 0);

    if (0 != (e = apr_pool_create(&p, 0))) {
	gpcc_warning(FLINE, "apr_pool_create failed");
	return e;
    }

    if (0 != (e = apr_queue_create(&alertq, 100, p))){
	gpcc_warning(FLINE, "apr_queue_create failed");
	return e;
    }

    if (0 != (e = apr_queue_create(&outq, 100, p))){
	gpcc_warning(FLINE, "apr_queue_create failed");
	return e;
    }

    fts_initialize(alertq);

    fts_do("start", outq);
    
    e = apr_queue_pop(outq, (void **) &out);
    if (e) {
	gpcc_warning(FLINE, "apr_queue_pop failed");
	return e;
    }

    printf("output = %s", out);
    fflush(stdout);
    
    fts_terminate();

    apr_terminate();
    return 0;
}
