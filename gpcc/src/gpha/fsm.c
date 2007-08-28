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
#include "gpha.h"

#include "fsm_http.h"
#include "fsm_system.h"
#include "fsm_segment.h"

APR_DECLARE(void) fsm_setup(void)
{
    gpcc_fsm_put(SYSTEM_CMDSTATE, TICK_EVENT, SYSTEM_CMDSTATE, 
		 a_system_tick, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_OPENED_EVENT, SYSTEM_CMDSTATE, 
		 a_system_net_opened, 0);
    gpcc_fsm_put(SYSTEM_CMDSTATE, NET_RECEIVED_EVENT, SYSTEM_CMDSTATE,
		 a_system_net_received, 0);

    /* http slot */
    gpcc_fsm_put(HTTP_REQUEST_CMDSTATE, INIT_EVENT, NULL_CMDSTATE,
		 http_request, 0);


    /* all slot -- get identity */
    gpcc_fsm_put(GET_IDENTITY_CMDSTATE, INIT_EVENT, GET_IDENTITY_CMDSTATE, 
		 a_get_identity_send_request, 0);
    gpcc_fsm_put(GET_IDENTITY_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE, 
		 a_close_conn, 0);


    /* ma slot */
    /* forward requests to segment */
    gpcc_fsm_put(GET_IDENTITY_CMDSTATE, SEGMENT_REQUEST_MSG_EVENT, GET_IDENTITY_CMDSTATE,
		 ma_dispatch_segment, 0);

    /* Segment FSM */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, INIT_EVENT, SEG_INIT_CMDSTATE,
		 seg_initialize, 0);

    /* segment initdb */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_INITDB_EVENT, SEG_INITDB_CMDSTATE,
		 seg_initdb, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_FAIL1_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_backout1, seg_initdb_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, TIMEUP_EVENT, SEG_INITDB_CMDSTATE,
		 seg_initdb_wait, 0);
    gpcc_fsm_put(SEG_INITDB_CMDSTATE, SEG_INITDB_NEXT_EVENT, SEG_INITDB_START_CMDSTATE,
		 seg_initdb_start_wait, 0);
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, SEG_INITDB_FAIL2_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_backout2, seg_initdb_reply, seg_request_cleanup, 0);		 
    gpcc_fsm_put(SEG_INITDB_START_CMDSTATE, SEG_INITDB_NEXT_EVENT, SEG_INITDB_CHECK_CMDSTATE,
		 seg_initdb_pg_conf_update, seg_initdb_pg_hba_append, seg_initdb_checkdb, 0);
    gpcc_fsm_put(SEG_INITDB_CHECK_CMDSTATE, SEG_INITDB_FAIL2_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_backout2, seg_initdb_reply, seg_request_cleanup, 0);
    gpcc_fsm_put(SEG_INITDB_CHECK_CMDSTATE, SEG_INITDB_DONE_EVENT, SEG_INIT_CMDSTATE,
		 seg_initdb_reply, seg_request_cleanup, 0);


    /* segment start */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_START_EVENT, SEG_START_CMDSTATE,
		 seg_start, 0);
    gpcc_fsm_put(SEG_START_CMDSTATE, TIMEUP_EVENT, SEG_START_CMDSTATE,
		 seg_start_wait, 0);
    gpcc_fsm_put(SEG_START_CMDSTATE, SEG_START_DONE_EVENT, SEG_INIT_CMDSTATE,
		 seg_start_reply, seg_request_cleanup, 0);

    /* segment stop */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_STOP_EVENT, SEG_STOP_CMDSTATE,
		 seg_stop, 0);
    gpcc_fsm_put(SEG_STOP_CMDSTATE, TIMEUP_EVENT, SEG_STOP_CMDSTATE,
		 seg_stop_wait, 0);
    gpcc_fsm_put(SEG_STOP_CMDSTATE, SEG_STOP_DONE_EVENT, SEG_INIT_CMDSTATE,
		 seg_stop_reply, seg_request_cleanup, 0);
    
    /* segment status */
    gpcc_fsm_put(SEG_INIT_CMDSTATE, SEG_STATUS_EVENT, SEG_INIT_CMDSTATE,
		 seg_status, 0);
		 

}
