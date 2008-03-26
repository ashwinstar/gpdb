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

#include "fsm_ma.h"
#include "fsm_init.h"
#include "fsm_stop.h"
#include "fsm_start.h"
#include "fsm_delete.h"
#include "fsm_status.h"

APR_DECLARE(void) fsm_setup(void)
{
    gpcc_fsm_put(SYSTEM_CMDSTATE, TICK_EVENT, SYSTEM_CMDSTATE, 
		 a_system_tick, NULL);

    /* ma slot */
    /* forward requests to segment */
    gpcc_fsm_put(MA_CMDSTATE, INIT_EVENT, MA_CMDSTATE,
		 NULL);
    gpcc_fsm_put(MA_CMDSTATE, SEGMENT_REQUEST_MSG_EVENT, MA_CMDSTATE,
		 ma_dispatch_segment, NULL);
    gpcc_fsm_put(MA_CMDSTATE, STATUS_REQUEST_MSG_EVENT, MA_CMDSTATE,
		 ma_dispatch_segment, NULL);
    gpcc_fsm_put(MA_CMDSTATE, SEG_STATUS_REPLY_EVENT, MA_CMDSTATE,
		 ma_status_reply, NULL);
    gpcc_fsm_put(MA_CMDSTATE, SEG_INITDB_REPLY_EVENT, MA_CMDSTATE,
		 ma_initdb_reply, NULL);
    gpcc_fsm_put(MA_CMDSTATE, SEG_START_REPLY_EVENT, MA_CMDSTATE,
		 ma_start_reply, NULL);
    gpcc_fsm_put(MA_CMDSTATE, SEG_STOP_REPLY_EVENT, MA_CMDSTATE,
		 ma_stop_reply, NULL);
    gpcc_fsm_put(MA_CMDSTATE, SEG_DELETE_REPLY_EVENT, MA_CMDSTATE,
		 ma_delete_reply, NULL);
    gpcc_fsm_put(MA_CMDSTATE, NET_CLOSED_EVENT, NULL_CMDSTATE,
		 ma_cleanup, NULL);

    /* segment initialize */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 NULL);

    /* segment initdb */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_INITDB_EVENT, SEG_INITDB_CMDSTATE,
		 seg_initialize, seg_initdb, NULL);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_FAIL0_EVENT, NULL_CMDSTATE,
		 seg_initdb_reply, seg_request_cleanup, NULL);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_FAIL1_EVENT, NULL_CMDSTATE,
		 seg_initdb_backout1, seg_initdb_reply, seg_request_cleanup, NULL);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, TIMEUP_EVENT, SEG_INITDB_CMDSTATE,
		 seg_initdb_wait, NULL);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_DONE_EVENT, NULL_CMDSTATE,
		 seg_initdb_reply, seg_request_cleanup, NULL);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_NEXT_EVENT, SEG_INITDB_START_CMDSTATE,
		 seg_initdb_start_wait, NULL);
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, TIMEUP_EVENT, SEG_INITDB_START_CMDSTATE,
		 seg_initdb_start_wait, NULL);
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, SEG_INITDB_FAIL2_EVENT, NULL_CMDSTATE,
		 seg_initdb_backout2, seg_initdb_backout1, seg_initdb_reply, seg_request_cleanup, NULL);		 
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, SEG_INITDB_NEXT_EVENT, SEG_INITDB_CHECK_CMDSTATE,
		 seg_initdb_checkdb, NULL);
    gpcc_fsm_put(SEG_INITDB_CHECK_CMDSTATE, SEG_INITDB_FAIL2_EVENT, NULL_CMDSTATE,
		 seg_initdb_backout2, seg_initdb_backout1, seg_initdb_reply, seg_request_cleanup, NULL);
    gpcc_fsm_put(SEG_INITDB_CHECK_CMDSTATE, SEG_INITDB_DONE_EVENT, NULL_CMDSTATE,
		 seg_initdb_reply, seg_request_cleanup, NULL);


    /* segment starting */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_START_EVENT, SEG_STARTING_CMDSTATE,
		 seg_initialize, seg_start, NULL);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, TIMEUP_EVENT, SEG_STARTING_CMDSTATE,
		 seg_start_wait, NULL);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 seg_start_reply, seg_request_cleanup, NULL);
    gpcc_fsm_put(SEG_STARTING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 seg_start_reply, seg_request_cleanup, NULL);

    /* segment stop */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_STOP_EVENT, SEG_STOPPING_CMDSTATE,
		 seg_initialize, seg_stop, NULL);
    gpcc_fsm_put(SEG_STOPPING_CMDSTATE, TIMEUP_EVENT, SEG_STOPPING_CMDSTATE,
		 seg_stop_wait, NULL);
    gpcc_fsm_put(SEG_STOPPING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 seg_stop_reply, seg_request_cleanup, NULL);
    gpcc_fsm_put(SEG_STOPPING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 seg_stop_reply, seg_request_cleanup, NULL);
    
    /* segment status */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_STATUS_EVENT, NULL_CMDSTATE,
		 seg_initialize, seg_status, NULL);

    /* segment delete */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_DELETE_EVENT, SEG_DELETING_CMDSTATE,
		 seg_initialize, seg_delete, NULL);
    gpcc_fsm_put(SEG_DELETING_CMDSTATE, TIMEUP_EVENT, SEG_DELETING_CMDSTATE,
		 seg_delete_wait, NULL);
    gpcc_fsm_put(SEG_DELETING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 seg_delete_rmdir, seg_delete_reply, seg_request_cleanup, NULL);
    gpcc_fsm_put(SEG_DELETING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 seg_delete_reply, seg_request_cleanup, NULL);

}
