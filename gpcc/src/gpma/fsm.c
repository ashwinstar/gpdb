#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpma.h"


apr_status_t a_system_tick(gpcc_event_t* ev, void* arg)
{
    printf("TICK\n");
    return 0;
}


APR_DECLARE(void) fsm_init(void)
{
    gpcc_fsm_put(GPMA_CMDSTATE, TICK_EVENT, GPMA_CMDSTATE, 
		 a_system_tick, 0);
#if 0
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_OPENED_EVENT, SYSTEM_CMDSTATE, 
		 a_system_net_opened, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_RECEIVED_EVENT, SYSTEM_CMDSTATE,
		 a_system_net_received, 0);

    gpcc_fsm_put(GET_IDENTITY_CMDSTATE, INIT_EVENT, GET_IDENTITY_CMDSTATE, 
		 a_get_identity_send_request, 0);
    gpcc_fsm_put(GET_IDENTITY_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE, 
		 a_close_conn, 0);
    
    gpcc_fsm_put(HTTP_REQUEST_CMDSTATE, INIT_EVENT, NULL_CMDSTATE,
		 http_request, 0);
#endif
}
