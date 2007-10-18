#include <apr_strings.h>
#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <gpcc_proc.h>
#include <gpcc_pg.h>
#include <gpcc_json.h>
#include <gpcc_file.h>
#include <gpcc_strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <json/json.h>
#include "gpha.h"

#include "fsm_system.h"
#include "fsm_segment.h"

APR_DECLARE(void) fsm_setup(void)
{
    gpcc_fsm_put(SYSTEM_CMDSTATE, INIT_EVENT, SYSTEM_CMDSTATE,
		 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, TICK_EVENT, SYSTEM_CMDSTATE, 
		 a_system_tick, 0);

    /* ma slot */
    /* forward requests to segment */
    gpcc_fsm_put(MA_CMDSTATE, INIT_EVENT, MA_CMDSTATE,
		 0);
    gpcc_fsm_put(MA_CMDSTATE, SEGMENT_REQUEST_MSG_EVENT, MA_CMDSTATE,
		 ma_dispatch_segment, 0);
    gpcc_fsm_put(MA_CMDSTATE, STATUS_REQUEST_MSG_EVENT, MA_CMDSTATE,
		 ma_dispatch_segment, 0);
    gpcc_fsm_put(MA_CMDSTATE, SEG_STATUS_REPLY_EVENT, MA_CMDSTATE,
		 ma_status_reply, 0);
    gpcc_fsm_put(MA_CMDSTATE, SEG_INITDB_REPLY_EVENT, MA_CMDSTATE,
		 ma_initdb_reply, 0);
    gpcc_fsm_put(MA_CMDSTATE, SEG_START_REPLY_EVENT, MA_CMDSTATE,
		 ma_start_reply, 0);
    gpcc_fsm_put(MA_CMDSTATE, SEG_STOP_REPLY_EVENT, MA_CMDSTATE,
		 ma_stop_reply, 0);
    gpcc_fsm_put(MA_CMDSTATE, NET_CLOSED_EVENT, NULL_CMDSTATE,
		 ma_cleanup, 0);

    /* Segment FSM */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, INIT_EVENT, SEG_INIT_CMDSTATE,
		 seg_initialize, 0);
    gpcc_fsm_put(SEG_INIT_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 seg_cleanup, 0);

    /* segment initdb */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_INITDB_EVENT, SEG_INITDB_CMDSTATE,
		 seg_initdb, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_FAIL0_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_FAIL1_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_backout1, seg_initdb_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, TIMEUP_EVENT, SEG_INITDB_CMDSTATE,
		 seg_initdb_wait, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_DONE_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_NEXT_EVENT, SEG_INITDB_START_CMDSTATE,
		 seg_initdb_start_wait, 0);
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, TIMEUP_EVENT, SEG_INITDB_START_CMDSTATE,
		 seg_initdb_start_wait, 0);
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, SEG_INITDB_FAIL2_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_backout2, seg_initdb_backout1, seg_initdb_reply, seg_request_cleanup, 0);		 
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, SEG_INITDB_NEXT_EVENT, SEG_INITDB_CHECK_CMDSTATE,
		 seg_initdb_checkdb, 0);
    gpcc_fsm_put(SEG_INITDB_CHECK_CMDSTATE, SEG_INITDB_FAIL2_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_backout2, seg_initdb_backout1, seg_initdb_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_INITDB_CHECK_CMDSTATE, SEG_INITDB_DONE_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_reply, seg_request_cleanup, 0);



    /* segment init */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_START_EVENT, SEG_STARTING_CMDSTATE,
		 seg_start, 0);
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_STOP_EVENT, SEG_STOPPING_CMDSTATE,
		 seg_stop, 0);

    /* segment stopped */
    gpcc_fsm_put(SEG_STOPPED_CMDSTATE, SEG_START_EVENT, SEG_STARTING_CMDSTATE,
		 seg_start, 0);
    gpcc_fsm_put(SEG_STOPPED_CMDSTATE, SEG_STOP_EVENT, SEG_STOPPED_CMDSTATE,
		 seg_busy_ok_reply, 0);

    /* segment starting */
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, SEG_START_EVENT, SEG_STARTING_CMDSTATE,
		 seg_busy_ok_reply, 0);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, SEG_STOP_EVENT, SEG_STARTING_CMDSTATE,
		 seg_busy_fail_reply, 0);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, TIMEUP_EVENT, SEG_STARTING_CMDSTATE,
		 seg_start_wait, 0);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, SUCCESS_EVENT, SEG_RUNNING_CMDSTATE,
		 seg_start_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, FAILURE_EVENT, SEG_STOPPED_CMDSTATE,
		 seg_start_reply, seg_request_cleanup, 0);

    /* segment running */
    gpcc_fsm_put(SEG_RUNNING_CMDSTATE, SEG_START_EVENT, SEG_RUNNING_CMDSTATE,
		 seg_busy_ok_reply, 0);
    gpcc_fsm_put(SEG_RUNNING_CMDSTATE, SEG_STOP_EVENT, SEG_STOPPING_CMDSTATE,
		 seg_stop, 0);

    /* segment stop */
    gpcc_fsm_put(SEG_STOPPING_CMDSTATE, TIMEUP_EVENT, SEG_STOPPING_CMDSTATE,
		 seg_stop_wait, 0);
    gpcc_fsm_put(SEG_STOPPING_CMDSTATE, SUCCESS_EVENT, SEG_STOPPED_CMDSTATE,
		 seg_stop_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_STOPPING_CMDSTATE, FAILURE_EVENT, SEG_STOPPED_CMDSTATE,
		 seg_stop_reply, seg_request_cleanup, 0);
    
    /* segment status */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_STATUS_EVENT, SEG_INIT_CMDSTATE,
		 seg_status, 0);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, SEG_STATUS_EVENT, SEG_STARTING_CMDSTATE,
		 seg_status, 0);
    gpcc_fsm_put(SEG_RUNNING_CMDSTATE, SEG_STATUS_EVENT, SEG_RUNNING_CMDSTATE,
		 seg_status, 0);
    gpcc_fsm_put(SEG_STOPPING_CMDSTATE, SEG_STATUS_EVENT, SEG_STOPPING_CMDSTATE,
		 seg_status, 0);
    gpcc_fsm_put(SEG_STOPPED_CMDSTATE, SEG_STATUS_EVENT, SEG_STOPPED_CMDSTATE,
		 seg_status, 0);
		 

}
