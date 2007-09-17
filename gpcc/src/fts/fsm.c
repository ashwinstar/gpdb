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
    gpcc_fsm_put(SYSTEM_CMDSTATE, INIT_EVENT, SYSTEM_CMDSTATE,
		 ha_connect_all, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_OPENED_EVENT, SYSTEM_CMDSTATE,
		 ha_opened, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_CMDSTATE,
		 ha_closed, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 setup_ready, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 setup_failure, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, SETUP_SSH_EVENT, SETUP_SSH_CMDSTATE,
		 setup_ssh_start, 0);
    
    gpcc_fsm_put(SETUP_SSH_CMDSTATE, TIMEUP_EVENT, SETUP_SSH_CMDSTATE,
		 setup_ssh_check, 0);
    gpcc_fsm_put(SETUP_SSH_CMDSTATE, SUCCESS_EVENT, SETUP_NEXT_CMDSTATE,
		 ha_connect_all, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, NET_OPENED_EVENT, SETUP_NEXT_CMDSTATE,
		 ha_opened, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 setup_failure, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 setup_ready, 0);


    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 ha_closed, 0);
    gpcc_fsm_put(SYSTEM_FATAL_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 ha_closed, 0);

    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, ARRAY_INIT_EVENT, INITDB_CMDSTATE,
		 initdb_do, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, INITDB_CMDSTATE,
		 initdb_reply, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, NET_FAILURE_EVENT, NULL_CMDSTATE,
		 ha_closed, initdb_reply, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 initdb_success, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 initdb_failure, 0);
    

}
