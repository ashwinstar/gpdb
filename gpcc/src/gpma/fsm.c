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
#include "fsm_ma.h"
#include "fsm_fts.h"
#include "fsm_initdb.h"
#include "fsm_start.h"
#include "fsm_status.h"
#include "fsm_stop.h"
#include "fsm_array_start.h"
#include "fsm_array_init.h"
#include "fsm_array_stop.h"


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
    gpcc_fsm_put(NODE_OFF_CMDSTATE, TIMEUP_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_ssh, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, SUCCESS_EVENT, NODE_CONNWAIT_CMDSTATE,
		 a_node_connect, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, TIMEUP_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_check_ssh, 0);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, NET_OPENED_EVENT, NODE_ON_CMDSTATE,
		 a_node_ready, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, TIMEUP_EVENT, NODE_CONNWAIT_CMDSTATE, 
		 a_node_check_conn, 0);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, 0);
    gpcc_fsm_put(NODE_ON_CMDSTATE, TIMEUP_EVENT, NODE_ON_CMDSTATE,
		 a_node_ping, 0);
    gpcc_fsm_put(NODE_ON_CMDSTATE, NET_CLOSED_EVENT, NODE_OFF_CMDSTATE, 
		 a_node_teardown, 0);

    /* ma */
    gpcc_fsm_put(MA_OFF_CMDSTATE, TIMEUP_EVENT, MA_CONNWAIT_CMDSTATE,
		 a_ma_connect, 0);
    gpcc_fsm_put(MA_CONNWAIT_CMDSTATE, NET_OPENED_EVENT, MA_ON_CMDSTATE, 
		 a_ma_ready, 0);
    gpcc_fsm_put(MA_CONNWAIT_CMDSTATE, NET_CLOSED_EVENT, MA_OFF_CMDSTATE,
		 a_ma_teardown, 0);
    gpcc_fsm_put(MA_ON_CMDSTATE, NET_CLOSED_EVENT, MA_OFF_CMDSTATE,
		 a_ma_teardown);

    /* lma */
    gpcc_fsm_put(LMA_OFF_CMDSTATE, INIT_EVENT, LMA_WAIT_CMDSTATE,
		 a_lma_init, 0);
    gpcc_fsm_put(LMA_WAIT_CMDSTATE, NET_OPENED_EVENT, LMA_WAIT_CMDSTATE,
		 a_lma_ma_opened, 0);
    gpcc_fsm_put(LMA_WAIT_CMDSTATE, NET_CLOSED_EVENT, LMA_WAIT_CMDSTATE,
		 a_lma_ma_closed, 0);
    gpcc_fsm_put(LMA_WAIT_CMDSTATE, SUCCESS_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_leadless, 0);


    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, NET_OPENED_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_ma_opened, 0);
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, NET_CLOSED_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_ma_closed, 0);

    gpcc_fsm_put(LMA_VOTED_CMDSTATE, NET_OPENED_EVENT, LMA_VOTED_CMDSTATE,
		 a_lma_ma_opened, 0);
    gpcc_fsm_put(LMA_VOTED_CMDSTATE, NET_CLOSED_EVENT, LMA_VOTED_CMDSTATE,
		 a_lma_ma_closed, 0);

    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, NET_OPENED_EVENT, LMA_VOTEME_CMDSTATE,
		 a_lma_ma_opened, 0);
    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, NET_CLOSED_EVENT, LMA_VOTEME_CMDSTATE,
		 a_lma_ma_closed, 0);

    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, NET_OPENED_EVENT, LMA_STANDBY_CMDSTATE,
		 a_lma_ma_opened, 0);
    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, NET_CLOSED_EVENT, LMA_STANDBY_CMDSTATE,
		 a_lma_ma_closed, 0);

    gpcc_fsm_put(LMA_LEAD_CMDSTATE, NET_OPENED_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_ma_opened, 0);
    gpcc_fsm_put(LMA_LEAD_CMDSTATE, NET_CLOSED_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_ma_closed, 0);    

    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, LMA_VOTE_ME_MSG_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_vote_requested, 0); /* think about voting yes or no, if yes: gen SUCCESS event */
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, TIMEUP_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_ponder_elect, 0); /* think about electing myself, if yes: gen ELECTSELF event */
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, LMA_VOTED_EVENT, LMA_VOTED_CMDSTATE, 0);
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, LMA_ELECTSELF_EVENT, LMA_VOTEME_CMDSTATE, 
		 a_lma_vote_me, 0);
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, SUCCESS_EVENT, LMA_LEAD_CMDSTATE, 0);
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, FAILURE_EVENT, LMA_LEADLESS_CMDSTATE, 
		 a_lma_leadless, 0);

    gpcc_fsm_put(LMA_VOTED_CMDSTATE, LMA_VOTE_ME_MSG_EVENT, LMA_VOTED_CMDSTATE,
		 a_lma_reject, 0); /* we voted for another ma, reject this. */
    gpcc_fsm_put(LMA_VOTED_CMDSTATE, LMA_VOTE_WIN_MSG_EVENT, LMA_VOTED_CMDSTATE,
		 a_lma_check_win, 0); /* check if this is who we voted for, if yes: gen SUCCESS event */
    gpcc_fsm_put(LMA_VOTED_CMDSTATE, LMA_VOTE_LOST_MSG_EVENT, LMA_VOTED_CMDSTATE,
		 a_lma_check_lost, 0); /* check if this is who we voted for, if yes: gen FAILURE event */
    gpcc_fsm_put(LMA_VOTED_CMDSTATE, FAILURE_EVENT, LMA_LEADLESS_CMDSTATE, 
		 a_lma_leadless, 0);
    gpcc_fsm_put(LMA_VOTED_CMDSTATE, TIMEUP_EVENT, LMA_LEADLESS_CMDSTATE, 
		 a_lma_leadless, 0);
    gpcc_fsm_put(LMA_VOTED_CMDSTATE, SUCCESS_EVENT, LMA_STANDBY_CMDSTATE, 0);
    
    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, FAILURE_EVENT, LMA_LEADLESS_CMDSTATE, 
		 a_lma_leadless, 0);

    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, LMA_VOTE_YES_MSG_EVENT, LMA_VOTEME_CMDSTATE, 
		 a_lma_ponder_vote, 0);
    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, LMA_VOTE_NO_MSG_EVENT, LMA_VOTEME_CMDSTATE, 
		 a_lma_ponder_vote, 0);
    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, LMA_VOTE_ME_MSG_EVENT, LMA_VOTEME_CMDSTATE,
		 a_lma_reject, 0);
    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, FAILURE_EVENT, LMA_LEADLESS_CMDSTATE, 
		 a_lma_leadless, 0);
    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, TIMEUP_EVENT, LMA_LEADLESS_CMDSTATE, 
		 a_lma_leadless, 0);
    gpcc_fsm_put(LMA_VOTEME_CMDSTATE, SUCCESS_EVENT, LMA_LEAD_CMDSTATE, 
		 a_lma_lead, 0);

    gpcc_fsm_put(LMA_LEAD_CMDSTATE, FAILURE_EVENT, LMA_LEADLESS_CMDSTATE, 
		 a_lma_lead_failure, a_lma_leadless, 0);


    /* fts */
    /*
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_STOP_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_stop, 0);
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_INIT_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_init, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_init_reply, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, SUCCESS_EVENT, FTS_CMDSTATE,
		 a_fts_initialized, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 a_fts_failure, 0);
    */
    gpcc_fsm_put(FTS_CMDSTATE, INIT_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_init, 0);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, NET_OPENED_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_node_opened, 0);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, NET_CLOSED_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_node_closed, 0);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, NET_OPENED_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_node_opened, 0);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, NET_CLOSED_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_node_closed, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, NET_OPENED_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_node_opened, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, NET_CLOSED_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_node_closed, 0);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, NET_OPENED_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_node_opened, 0);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, NET_CLOSED_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_node_closed, 0);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, NET_OPENED_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_node_opened, 0);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, NET_CLOSED_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_node_closed, 0);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, NET_OPENED_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_node_opened, 0);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, NET_CLOSED_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_node_closed, 0);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, NET_OPENED_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_node_opened, 0);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, NET_CLOSED_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_node_closed, 0);

    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_STATUS_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_array_status_reply, 0);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, SUCCESS_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_array_present, 0);


    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_ABSENT_EVENT, FTS_EMPTY_CMDSTATE,
		 0);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_INIT_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_init, 0);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_START_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_busy_reply, 0);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_STOP_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_busy_reply, 0);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_STATUS_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_array_status_reply, 0);


    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_init_reply, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, SUCCESS_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_array_init_success, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, FAILURE_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_array_init_failure, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_INIT_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_busy_ok_reply, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_START_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_busy_reply, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_START_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_busy_reply, 0);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_status_reply, 0);
    

    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_PRESENT_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_stop_forsure, 0);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_START_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_array_start, 0);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_STOP_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_busy_ok_reply, 0);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_INIT_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_busy_reply, 0);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_STATUS_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_array_status_reply, 0);    

    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_START_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_ok_reply, 0);    
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_STOP_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_reply, 0);    
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_INIT_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_reply, 0);    
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_array_status_reply, 0);    
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_array_start_reply, 0);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, SUCCESS_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_running, 0);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, FAILURE_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_failure, 0);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_STOP_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_stop, 0);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_array_status_reply, 0);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_START_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_busy_ok_reply, 0);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_INIT_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_busy_reply, 0);

    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_START_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_reply, 0);    
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_STOP_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_ok_reply, 0);  
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_INIT_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_reply, 0);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_status_reply, 0);    
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_stop_reply, 0);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, SUCCESS_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_stopped, 0);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, FAILURE_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_failure, 0);



    /* init */
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, INIT_EVENT, FTS_INITDB_CMDSTATE,
		 a_initdb_init, a_initdb_do, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_INITDB_CMDSTATE,
		 a_initdb_reply, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, CONTINUE_EVENT, FTS_INITDB_CMDSTATE,
		 a_initdb_do, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_initdb_success, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_initdb_failure, 0);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_initdb_timeout, 0);

    /* start */
    gpcc_fsm_put(FTS_START_CMDSTATE, INIT_EVENT, FTS_START_CMDSTATE,
		 a_start_do, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_START_CMDSTATE,
		 a_start_reply, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_start_success, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_start_failure, 0);
    gpcc_fsm_put(FTS_START_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_start_timeout, 0);

    /* stop */
    
    gpcc_fsm_put(FTS_STOP_CMDSTATE, INIT_EVENT, FTS_STOP_CMDSTATE,
		 a_stop_do, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_STOP_CMDSTATE,
		 a_stop_reply, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_stop_success, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_stop_failure, 0);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_stop_timeout, 0);

 
   /* status */
    
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, INIT_EVENT, FTS_STATUS_CMDSTATE,
		 a_status_do, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, STATUS_REPLY_MSG_EVENT, FTS_STATUS_CMDSTATE,
		 a_status_reply, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_status_success, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_status_failure, 0);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_status_timeout, 0);

    /* array stop */
    gpcc_fsm_put(FTS_ARRAY_STOP_CMDSTATE, INIT_EVENT, FTS_ARRAY_STOP_WAIT_CMDSTATE,
		 a_array_stop_do, 0);
    gpcc_fsm_put(FTS_ARRAY_STOP_WAIT_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_ARRAY_STOP_WAIT_CMDSTATE,
		 a_array_stop_units_stop_reply, 0);
    gpcc_fsm_put(FTS_ARRAY_STOP_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_array_stop_success, 0);
    gpcc_fsm_put(FTS_ARRAY_STOP_WAIT_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_array_stop_failure, 0);

    /* array initdb */
    gpcc_fsm_put(FTS_ARRAY_INIT_CMDSTATE, INIT_EVENT, FTS_ARRAY_INIT_WAIT_CMDSTATE,
		 a_array_init_do, 0);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_ARRAY_INIT_WAIT_CMDSTATE,
		 a_array_init_units_init_reply, 0);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_array_init_success, 0);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_array_init_failure, 0);
		 

    /* array start */
    /* start qd in utility mode */
    gpcc_fsm_put(FTS_ARRAY_START_CMDSTATE, INIT_EVENT, FTS_START_UTILITY_QD_CMDSTATE,
		 a_array_start_utility_qd, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_UTILITY_QD_CMDSTATE,
		 a_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, SUCCESS_EVENT, FTS_START_GPCONF_CMDSTATE,
		 a_array_start_gpconf, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, 0);

    /* update gp_configuration */
    gpcc_fsm_put(FTS_START_GPCONF_CMDSTATE, SUCCESS_EVENT, FTS_START_UTILITY_QD_STOP_CMDSTATE,
		 a_array_start_utility_qd_stop, 0);
    gpcc_fsm_put(FTS_START_GPCONF_CMDSTATE, FAILURE_EVENT, FTS_START_GPCONF_FAILURE_CMDSTATE,
		 a_array_start_utility_qd_stop, 0);

    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_GPCONF_FAILURE_CMDSTATE,
		 a_array_start_units_stop_reply, 0);
    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, 0);
    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, 0);

    /* stop qd */
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_UTILITY_QD_STOP_CMDSTATE,
		 a_array_start_units_stop_reply, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, SUCCESS_EVENT, FTS_START_QES_CMDSTATE,
		 a_array_start_qes, 0);
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, 0);

    /* start all qes */
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_QES_CMDSTATE,
		 a_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, SUCCESS_EVENT, FTS_START_QD_CMDSTATE,
		 a_array_start_qd, 0);
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, 0);
		 
    /* start qd */
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_QD_CMDSTATE,
		 a_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, SUCCESS_EVENT, FTS_START_STANDBY_QD_CMDSTATE,
		 a_array_start_standby_qd, 0);
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, 0);

    /* start standby qd */
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_STANDBY_QD_CMDSTATE,
		 a_array_start_units_start_reply, 0);
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_array_start_success, a_array_start_cleanup, 0);
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, 0);

}
