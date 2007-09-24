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
#include "fsm_stopdb.h"
#include "fsm_status.h"
#include "fsm_start.h"

APR_DECLARE(void) fsm_setup(void)
{

    gpcc_fsm_put(SYSTEM_CMDSTATE, INIT_EVENT, SYSTEM_CMDSTATE,
		 system_init, 0);

    /* setup */
    gpcc_fsm_put(SYSTEM_CMDSTATE, ARRAY_SETUP_EVENT, SYSTEM_CMDSTATE,
		 setup_init, setup_array_connect, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_OPENED_EVENT, SYSTEM_CMDSTATE,
		 setup_ha_opened, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_CLOSED_EVENT, SYSTEM_CMDSTATE,
		 ha_closed, setup_ha_closed, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 setup_ready, setup_cleanup, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 setup_failure, setup_array_cleanup, setup_cleanup, system_end, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, SETUP_SSH_EVENT, SETUP_SSH_CMDSTATE,
		 setup_ssh_start, 0);
    
    gpcc_fsm_put(SETUP_SSH_CMDSTATE, SUCCESS_EVENT, SETUP_NEXT_CMDSTATE,
		 setup_array_connect, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, NET_OPENED_EVENT, SETUP_NEXT_CMDSTATE,
		 setup_ha_opened, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 setup_failure, setup_array_cleanup, setup_cleanup, system_end, 0);
    gpcc_fsm_put(SETUP_NEXT_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 setup_ready, setup_cleanup, 0);


    /* network failure */
    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, NET_CLOSED_EVENT, SYSTEM_FATAL_CMDSTATE,
		 ha_closed, 0);
    gpcc_fsm_put(SYSTEM_FATAL_CMDSTATE, NET_CLOSED_EVENT, SYSTEM_FATAL_CMDSTATE,
		 ha_closed, 0);

    /* init */
    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, ARRAY_INIT_EVENT, INITDB_CMDSTATE,
		 initdb_init, initdb_do, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, INITDB_CMDSTATE,
		 initdb_reply, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 initdb_success, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 initdb_failure, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 initdb_failure, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, NET_CLOSED_EVENT, INITDB_CMDSTATE,
		 ha_closed, initdb_reply, 0);


    /* stop */
    
    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, ARRAY_STOP_EVENT, STOPDB_CMDSTATE,
		 stopdb_init, stopdb_do, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, STOPDB_CMDSTATE,
		 stopdb_reply, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 stopdb_success, stopdb_cleanup, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 stopdb_failure, stopdb_cleanup, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 stopdb_failure, stopdb_cleanup, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, NET_CLOSED_EVENT, STOPDB_CMDSTATE,
		 ha_closed, stopdb_reply, 0);

 
   /* status */
    
    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, ARRAY_STATUS_EVENT, STATUS_CMDSTATE,
		 status_init, status_do, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, STATUS_REPLY_MSG_EVENT, STATUS_CMDSTATE,
		 status_reply, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 status_success, status_cleanup, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 status_failure, status_cleanup, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 status_failure, status_cleanup, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, NET_CLOSED_EVENT, STATUS_CMDSTATE,
		 ha_closed, status_reply, 0);


    /* start */
    /* start qd in utility mode */
    gpcc_fsm_put(SYSTEM_READY_CMDSTATE, ARRAY_START_EVENT, START_UTILITY_QD_CMDSTATE,
		 start_init, start_utility_qd, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_UTILITY_QD_CMDSTATE,
		 start_utility_qd_reply, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, SUCCESS_EVENT, START_GPCONF_CMDSTATE,
		 start_gpconf, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, NET_CLOSED_EVENT, START_UTILITY_QD_CMDSTATE,
		 ha_closed, start_utility_qd_reply, 0);

    /* update gp_configuration */
    gpcc_fsm_put(START_GPCONF_CMDSTATE, SUCCESS_EVENT, START_UTILITY_QD_STOP_CMDSTATE,
		 start_utility_qd_stop, 0);
    gpcc_fsm_put(START_GPCONF_CMDSTATE, FAILURE_EVENT, START_GPCONF_FAILURE_CMDSTATE,
		 start_utility_qd_stop, 0);
    gpcc_fsm_put(START_GPCONF_CMDSTATE, NET_CLOSED_EVENT, START_GPCONF_CMDSTATE,
		 ha_closed, 0);

    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, SYSTEM_READY_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, NET_CLOSED_EVENT, START_GPCONF_FAILURE_CMDSTATE,
		 ha_closed, start_gpconf_network_closed, 0);

    /* stop qd */
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_UTILITY_QD_STOP_CMDSTATE,
		 start_utility_qd_stop_reply, 0);
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, SUCCESS_EVENT, START_QES_CMDSTATE,
		 start_qes, 0);
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, NET_CLOSED_EVENT, START_UTILITY_QD_STOP_CMDSTATE,
		 ha_closed, start_utility_qd_stop_reply, 0);

    /* start all qes */
    gpcc_fsm_put(START_QES_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_QES_CMDSTATE,
		 start_qe_reply, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, SUCCESS_EVENT, START_QD_CMDSTATE,
		 start_qd, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, NET_CLOSED_EVENT, START_QES_CMDSTATE,
		 start_qe_reply, 0);
		 
    /* start qd */
    gpcc_fsm_put(START_QD_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_QD_CMDSTATE,
		 start_qd_reply, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, NET_CLOSED_EVENT, START_QD_CMDSTATE,
		 start_qd_reply, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, SUCCESS_EVENT, START_STANDBY_QD_CMDSTATE,
		 start_standby_qd, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);

    /* start standby qd */
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_STANDBY_QD_CMDSTATE,
		 start_standby_qd_reply, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, NET_CLOSED_EVENT, START_STANDBY_QD_CMDSTATE,
		 start_standby_qd_reply, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, SUCCESS_EVENT, SYSTEM_READY_CMDSTATE,
		 start_success, start_cleanup, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, FAILURE_EVENT, SYSTEM_READY_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
   
}
