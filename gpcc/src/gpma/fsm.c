#include <apr_strings.h>
#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <gpcc_json.h>
#include <gpcc_strings.h>
#include <gpcc_pg.h>
#include <gpcc_proc.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpma.h"

#include "fsm_node.h"
/*
#include "fsm_setup.h"
*/
/*
#include "fsm_initdb.h"
*/
#include "fsm_start.h"
#include "fsm_array_start.h"
/*
#include "fsm_status.h"
*/
#include "fsm_stop.h"
/*
#include "fsm_array_stop.h"
*/


apr_status_t a_system_tick(gpcc_event_t* ev, void* arg)
{
    cmdslot_tick();
    return 0;
}



APR_DECLARE(void) fsm_init(void)
{
    gpcc_fsm_put(GPMA_CMDSTATE, TICK_EVENT, GPMA_CMDSTATE, 
		 a_system_tick, 0);

    /* node */
    gpcc_fsm_put(NODE_OFF_CMDSTATE, INIT_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_ssh, 0);
    gpcc_fsm_put(NODE_OFF_CMDSTATE, TIMEUP_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_ssh, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, SUCCESS_EVENT, NODE_CONNWAIT_CMDSTATE,
		 a_node_connect, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, TIMEUP_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_check_ssh, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, NET_OPENED_EVENT, NODE_ON_CMDSTATE, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, TIMEUP_EVENT, NODE_CONNWAIT_CMDSTATE, 
		 a_node_check_conn, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, 0);
    gpcc_fsm_put(NODE_ON_CMDSTATE, TIMEUP_EVENT, NODE_ON_CMDSTATE,
		 a_node_ping, 0);
    gpcc_fsm_put(NODE_ON_CMDSTATE, NET_CLOSED_EVENT, NODE_OFF_CMDSTATE, 
		 a_node_broadcast_net_closed, a_node_teardown, 0);

    /* init */
    /*
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_INIT_EVENT, FTS_INITDB_CMDSTATE,
		 a_fts_setup_init, a_fts_initdb_init, a_fts_initdb_do, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_INITDB_CMDSTATE,
		 a_fts_initdb_reply, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, CONTINUE_EVENT, FTS_INITDB_CMDSTATE,
		 a_fts_initdb_do, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_fts_initdb_success, a_fts_initdb_cleanup, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_initdb_failure, a_fts_initdb_cleanup, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, NET_FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_initdb_failure, a_fts_initdb_cleanup, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, NET_CLOSED_EVENT, FTS_INITDB_CMDSTATE,
		 a_fts_initdb_reply, 0);
    */

    /* start */
    gpcc_fsm_put(FTS_START_CMDSTATE, INIT_EVENT, FTS_START_CMDSTATE,
		 a_fts_start_do, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_START_CMDSTATE,
		 a_fts_start_reply, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_fts_start_success, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_start_failure, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, NET_FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_start_failure, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_fts_start_timeout, 0);

    /* stop */
    
    gpcc_fsm_put(FTS_STOP_CMDSTATE, INIT_EVENT, FTS_STOP_CMDSTATE,
		 a_fts_stop_do, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_STOP_CMDSTATE,
		 a_fts_stop_reply, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_fts_stop_success, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_stop_failure, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, NET_FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_stop_failure, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_fts_stop_timeout, 0);

 
   /* status */
    /*
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_STATUS_EVENT, FTS_STATUS_CMDSTATE,
		 a_fts_setup_init, a_fts_status_init, a_fts_status_do, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, STATUS_REPLY_MSG_EVENT, FTS_STATUS_CMDSTATE,
		 a_fts_status_reply, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_fts_status_success, a_fts_status_cleanup, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_status_failure, a_fts_status_cleanup, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, NET_FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_status_failure, a_fts_status_cleanup, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, NET_CLOSED_EVENT, FTS_STATUS_CMDSTATE,
		 a_fts_status_reply, 0);
    */

    /* array start */
    /* start qd in utility mode */
    gpcc_fsm_put(FTS_ARRAY_START_CMDSTATE, INIT_EVENT, FTS_START_UTILITY_QD_CMDSTATE,
		 a_fts_array_start_utility_qd, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_UTILITY_QD_CMDSTATE,
		 a_fts_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, SUCCESS_EVENT, FTS_START_GPCONF_CMDSTATE,
		 a_fts_array_start_gpconf, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_failure, a_fts_array_start_cleanup, 0);

    /* update gp_configuration */
    gpcc_fsm_put(FTS_START_GPCONF_CMDSTATE, SUCCESS_EVENT, FTS_START_UTILITY_QD_STOP_CMDSTATE,
		 a_fts_array_start_utility_qd_stop, 0);
    gpcc_fsm_put(FTS_START_GPCONF_CMDSTATE, FAILURE_EVENT, FTS_START_GPCONF_FAILURE_CMDSTATE,
		 a_fts_array_start_utility_qd_stop, 0);

    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_GPCONF_FAILURE_CMDSTATE,
		 a_fts_array_start_units_stop_reply, 0);
    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_failure, a_fts_array_start_cleanup, 0);
    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_failure, a_fts_array_start_cleanup, 0);

    /* stop qd */
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_UTILITY_QD_STOP_CMDSTATE,
		 a_fts_array_start_units_stop_reply, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, SUCCESS_EVENT, FTS_START_QES_CMDSTATE,
		 a_fts_array_start_qes, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_failure, a_fts_array_start_cleanup, 0);

    /* start all qes */
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_QES_CMDSTATE,
		 a_fts_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, SUCCESS_EVENT, FTS_START_QD_CMDSTATE,
		 a_fts_array_start_qd, 0);
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_failure, a_fts_array_start_cleanup, 0);
		 
    /* start qd */
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_QD_CMDSTATE,
		 a_fts_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, SUCCESS_EVENT, FTS_START_STANDBY_QD_CMDSTATE,
		 a_fts_array_start_standby_qd, 0);
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_failure, a_fts_array_start_cleanup, 0);

    /* start standby qd */
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_STANDBY_QD_CMDSTATE,
		 a_fts_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_success, a_fts_array_start_cleanup, 0);
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_fts_array_start_failure, a_fts_array_start_cleanup, 0);

}
