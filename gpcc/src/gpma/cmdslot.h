#ifndef CMDSLOT_H
#define CMDSLOT_H

#include <gpcc.h>
#include "enum.h"


typedef struct cmdslot_t cmdslot_t;
struct cmdslot_t {
    apr_uint32_t cmdidx;
    const char*  cmdname;
    cmdstate_t   cmdstate;
    int          countdown;	/* if > 0, will count down to 0 
				   and generate a TIMEUP_EVENT */
    union {
	struct {
	    
	} a;
    } u;
};


APR_DECLARE(cmdslot_t*) cmdslot_alloc(const char* name, cmdstate_t state);
APR_DECLARE(cmdslot_t*) cmdslot_find(apr_uint32_t cmdidx);
APR_DECLARE(void) cmdslot_free(apr_uint32_t cmdidx);



#endif /* CMDSLOT_H */
