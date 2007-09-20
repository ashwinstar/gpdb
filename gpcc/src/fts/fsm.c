#include <apr_strings.h>
#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <gpcc_http.h>
#include <gpcc_proc.h>
#include <gpcc_pg.h>
#include <gpcc_json.h>
#include <gpcc_file.h>
#include <stdio.h>
#include <stdlib.h>
#include <json/json.h>
#include "gpfts.h"

#include "fsm_setup.h"
#include "fsm_initdb.h"


APR_DECLARE(void) fsm_setup(void)
{
    gpcc_fsm_put(TIMER_CMDSTATE, INIT_EVENT, TIMER_CMDSTATE,
		 timer_init, 0);
    gpcc_fsm_put(TIMER_CMDSTATE, TICK_EVENT, TIMER_CMDSTATE,
		 timer_tick, 0);

    gpcc_fsm_put(SYSTEM_CMDSTATE, INIT_EVENT, SYSTEM_CMDSTATE,
		 system_init, 0);

    gpcc_fsm_put(SYSTEM_CMDSTATE, ARRAY_SETUP_EVENT, SYSTEM_CMDSTATE,
		 setup_init, setup_array_connect, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_OPENED_EVENT, SYSTEM_CMDSTATE,
		 setup_ha_opened, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_CLOSED_EVENT, SYSTEM_CMDSTATE,
		 ha_closed, setup_ha_closed, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 setup_ready, setup_cleanup, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 setup_failure, setup_array_cleanup, setup_cleanup, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, SETUP_SSH_EVENT, SETUP_SSH_CMDSTATE,
		 setup_ssh_start, 0);
    
    gpcc_fsm_put(SETUP_SSH_CMDSTATE, SUCCESS_EVENT, SETUP_NEXT_CMDSTATE,
		 setup_array_connect, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, NET_OPENED_EVENT, SETUP_NEXT_CMDSTATE,
		 setup_ha_opened, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 setup_failure, setup_array_cleanup, setup_cleanup, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 setup_ready, setup_cleanup, 0);


    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, NET_CLOSED_EVENT, SYSTEM_FATAL_CMDSTATE,
		 ha_closed, 0);
    gpcc_fsm_put(SYSTEM_FATAL_CMDSTATE, NET_CLOSED_EVENT, SYSTEM_FATAL_CMDSTATE,
		 ha_closed, 0);

    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, ARRAY_INIT_EVENT, INITDB_CMDSTATE,
		 initdb_init, initdb_do, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, INITDB_CMDSTATE,
		 initdb_reply, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 initdb_success, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 initdb_failure, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, NET_CLOSED_EVENT, INITDB_CMDSTATE,
		 ha_closed, initdb_reply, 0);



}
