#include <apr_strings.h>
#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <gpcc_json.h>
#include <gpcc_strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpma.h"

#include "fsm_setup.h"
#include "fsm_initdb.h"
#include "fsm_start.h"
#include "fsm_status.h"
#include "fsm_stopdb.h"

apr_status_t a_system_tick(gpcc_event_t* ev, void* arg)
{
    printf("TICK\n");
    return 0;
}


apr_status_t a_node_ssh(gpcc_event_t* ev, void* arg)
{
    printf("NODE SSH\n");
    return 0;
}

apr_status_t a_node_check_ssh(gpcc_event_t* ev, void* arg)
{
    printf("NODE CHECK SSH\n");
    return 0;
}

apr_status_t a_node_connect(gpcc_event_t* ev, void* arg)
{
    printf("NODE CONNECT\n");
    return 0;
}

apr_status_t a_node_check_conn(gpcc_event_t* ev, void* arg)
{
    printf("NODE CHECK CONN\n");
    return 0;
}

apr_status_t a_node_teardown(gpcc_event_t* ev, void* arg)
{
    printf("NODE TEARDOWN\n");
    return 0;
}

apr_status_t a_node_ping(gpcc_event_t* ev, void* arg)
{
    printf("NODE PING\n");
    return 0;
}



APR_DECLARE(void) fsm_init(void)
{
    gpcc_fsm_put(GPMA_CMDSTATE, TICK_EVENT, GPMA_CMDSTATE, 
		 a_system_tick, 0);

    /* node */
    gpcc_fsm_put(NODE_OFF_CMDSTATE, INIT_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_ssh, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, SUCCESS_EVENT, NODE_CONNWAIT_CMDSTATE,
		 a_node_connect, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, TIMEUP_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_check_ssh, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, SUCCESS_EVENT, NODE_ON_CMDSTATE, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, TIMEUP_EVENT, NODE_CONNWAIT_CMDSTATE, 
		 a_node_check_conn, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, 0);
    gpcc_fsm_put(NODE_ON_CMDSTATE, TIMEUP_EVENT, NODE_ON_CMDSTATE,
		 a_node_ping, 0);
    gpcc_fsm_put(NODE_ON_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE, 
		 a_node_teardown, 0);

    /* init */
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_INIT_EVENT, INITDB_CMDSTATE,
		 setup_init, initdb_init, initdb_do, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, INITDB_CMDSTATE,
		 initdb_reply, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, CONTINUE_EVENT, INITDB_CMDSTATE,
		 initdb_do, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, SUCCESS_EVENT, FTS_CMDSTATE,
		 initdb_success, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 initdb_failure, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 initdb_failure, initdb_cleanup, 0);
    gpcc_fsm_put(INITDB_CMDSTATE, NET_CLOSED_EVENT, INITDB_CMDSTATE,
		 initdb_reply, 0);


    /* stop */
    
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_STOP_EVENT, STOPDB_CMDSTATE,
		 setup_init, stopdb_init, stopdb_do, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, STOPDB_CMDSTATE,
		 stopdb_reply, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, SUCCESS_EVENT, FTS_CMDSTATE,
		 stopdb_success, stopdb_cleanup, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 stopdb_failure, stopdb_cleanup, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 stopdb_failure, stopdb_cleanup, 0);
    gpcc_fsm_put(STOPDB_CMDSTATE, NET_CLOSED_EVENT, STOPDB_CMDSTATE,
		 stopdb_reply, 0);

 
   /* status */
    
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_STATUS_EVENT, STATUS_CMDSTATE,
		 setup_init, status_init, status_do, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, STATUS_REPLY_MSG_EVENT, STATUS_CMDSTATE,
		 status_reply, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, SUCCESS_EVENT, FTS_CMDSTATE,
		 status_success, status_cleanup, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 status_failure, status_cleanup, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 status_failure, status_cleanup, 0);
    gpcc_fsm_put(STATUS_CMDSTATE, NET_CLOSED_EVENT, STATUS_CMDSTATE,
		 status_reply, 0);


    /* start */
    /* start qd in utility mode */
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_START_EVENT, START_UTILITY_QD_CMDSTATE,
		 setup_init, start_init, start_utility_qd, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_UTILITY_QD_CMDSTATE,
		 start_utility_qd_segment_reply, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, SUCCESS_EVENT, START_GPCONF_CMDSTATE,
		 start_gpconf, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_UTILITY_QD_CMDSTATE, NET_CLOSED_EVENT, START_UTILITY_QD_CMDSTATE,
		 start_utility_qd_net_closed, 0);

    /* update gp_configuration */
    gpcc_fsm_put(START_GPCONF_CMDSTATE, SUCCESS_EVENT, START_UTILITY_QD_STOP_CMDSTATE,
		 start_utility_qd_stop, 0);
    gpcc_fsm_put(START_GPCONF_CMDSTATE, FAILURE_EVENT, START_GPCONF_FAILURE_CMDSTATE,
		 start_utility_qd_stop, 0);
    gpcc_fsm_put(START_GPCONF_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_GPCONF_CMDSTATE, NET_CLOSED_EVENT, START_GPCONF_CMDSTATE,
		 0);

    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_GPCONF_FAILURE_CMDSTATE,
		 start_utility_qd_stop_reply, 0);
    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, NET_CLOSED_EVENT, START_GPCONF_FAILURE_CMDSTATE,
		 start_gpconf_net_closed, 0);
    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, SUCCESS_EVENT, FTS_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_GPCONF_FAILURE_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 start_failure, start_cleanup, 0);

    /* stop qd */
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_UTILITY_QD_STOP_CMDSTATE,
		 start_utility_qd_stop_reply, 0);
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, SUCCESS_EVENT, START_QES_CMDSTATE,
		 start_qes, 0);
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_UTILITY_QD_STOP_CMDSTATE, NET_CLOSED_EVENT, START_UTILITY_QD_STOP_CMDSTATE,
		 start_utility_qd_stop_reply, 0);

    /* start all qes */
    gpcc_fsm_put(START_QES_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_QES_CMDSTATE,
		 start_qe_reply, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, SUCCESS_EVENT, START_QD_CMDSTATE,
		 start_qd, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_QES_CMDSTATE, NET_CLOSED_EVENT, START_QES_CMDSTATE,
		 start_qe_reply, 0);
		 
    /* start qd */
    gpcc_fsm_put(START_QD_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_QD_CMDSTATE,
		 start_qd_reply, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, NET_CLOSED_EVENT, START_QD_CMDSTATE,
		 start_qd_reply_net_closed, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, SUCCESS_EVENT, START_STANDBY_QD_CMDSTATE,
		 start_standby_qd, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_QD_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);

    /* start standby qd */
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, START_STANDBY_QD_CMDSTATE,
		 start_standby_qd_reply, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, NET_CLOSED_EVENT, START_STANDBY_QD_CMDSTATE,
		 start_standby_qd_reply_net_closed, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, SUCCESS_EVENT, FTS_CMDSTATE,
		 start_success, start_cleanup, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 start_failure, start_cleanup, 0);
    gpcc_fsm_put(START_STANDBY_QD_CMDSTATE, NET_FAILURE_EVENT, SYSTEM_FATAL_CMDSTATE,
		 start_failure, start_cleanup, 0);

}
