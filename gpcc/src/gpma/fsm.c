#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include <apr_time.h>
#include <gpcc.h>
#include <gpcc_fsm.h>
#include <gpcc_net.h>
#include <gpcc_json.h>
#include <gpcc_strings.h>
#include <gpcc_pg.h>
#include <gpcc_proc.h>
#include <gpcc_file.h>
#include <chubby.h>
#include "gpma.h"

#define OK_STATUS                                    "ok"
#define FAIL_STATUS                                  "error"

#include "fsm_chubby.h"
#include "fsm_node.h"
#include "fsm_admin.h"
#include "fsm_login.h"
#include "fsm_ma.h"
#include "fsm_config.h"
#include "fsm_fts.h"
#include "fsm_haset.h"
#include "fsm_initdb.h"
#include "fsm_start.h"
#include "fsm_status.h"
#include "fsm_stop.h"
#include "fsm_delete.h"
#include "fsm_array_start.h"
#include "fsm_array_init.h"
#include "fsm_array_stop.h"
#include "fsm_array_delete.h"
#include "fsm_unit.h"
#include "fsm_segment_start.h"
#include "fsm_segment_stop.h"
#include "fsm_ssh.h"

apr_status_t a_system_tick(gpcc_event_t* ev, void* arg)
{
    cmdslot_tick();
    return 0;
}



APR_DECLARE(void) fsm_init(void)
{
    gpcc_fsm_put(GPMA_CMDSTATE, TICK_EVENT, GPMA_CMDSTATE, 
		 a_system_tick, NULL);

    /* configuration */
    gpcc_fsm_put(CFG_CMDSTATE, CFG_UPDATED_EVENT, CFG_CMDSTATE,
		 a_config_updated, NULL);
    gpcc_fsm_put(CFG_CMDSTATE, TIMEUP_EVENT, CFG_CMDSTATE,
		 a_config_updated, NULL);
    gpcc_fsm_put(CFG_CMDSTATE, CHUBBY_PUT_REPLY_EVENT, CFG_CMDSTATE,
		 a_config_put_reply, NULL);
    gpcc_fsm_put(CFG_CMDSTATE, CHUBBY_REMOVE_REPLY_EVENT, CFG_CMDSTATE,
		 a_config_remove_reply, NULL);
    gpcc_fsm_put(CFG_CMDSTATE, ARRAY_CONFIG_EVENT, CFG_CMDSTATE,
		 a_config_list, NULL);

    /* node */
    gpcc_fsm_put(NODE_CMDSTATE, INIT_EVENT, NODE_OFF_CMDSTATE,
		 a_node_init, NULL);
    gpcc_fsm_put(NODE_OFF_CMDSTATE, TIMEUP_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_ssh, NULL);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, SUCCESS_EVENT, NODE_CONNWAIT_CMDSTATE,
		 a_node_connect, NULL);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, TIMEUP_EVENT, NODE_SSHWAIT_CMDSTATE,
		 a_node_check_ssh, NULL);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, NULL);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, NET_OPENED_EVENT, NODE_ON_CMDSTATE,
		 a_node_ready, NULL);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, TIMEUP_EVENT, NODE_CONNWAIT_CMDSTATE, 
		 a_node_check_conn, NULL);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, NET_CLOSED_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, NULL);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, FAILURE_EVENT, NODE_OFF_CMDSTATE,
		 a_node_teardown, NULL);
    gpcc_fsm_put(NODE_ON_CMDSTATE, TIMEUP_EVENT, NODE_ON_CMDSTATE,
		 a_node_ping, NULL);
    gpcc_fsm_put(NODE_ON_CMDSTATE, NET_CLOSED_EVENT, NODE_OFF_CMDSTATE, 
		 a_node_teardown, NULL);
    gpcc_fsm_put(NODE_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE,
		 a_node_shutdown, NULL);
    gpcc_fsm_put(NODE_OFF_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE,
		 a_node_shutdown, NULL);
    gpcc_fsm_put(NODE_SSHWAIT_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE,
		 a_node_shutdown, NULL);
    gpcc_fsm_put(NODE_CONNWAIT_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE,
		 a_node_shutdown, NULL);
    gpcc_fsm_put(NODE_ON_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE,
		 a_node_shutdown, NULL);


    /* lma */
    gpcc_fsm_put(LMA_OFF_CMDSTATE, TIMEUP_EVENT, LMA_MKDIR_CMDSTATE,
		 a_lma_init, a_lma_mkdir, NULL);
    gpcc_fsm_put(LMA_MKDIR_CMDSTATE, SUCCESS_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_get_lease, NULL);
    gpcc_fsm_put(LMA_MKDIR_CMDSTATE, TIMEUP_EVENT, LMA_MKDIR_CMDSTATE,
		 a_lma_mkdir, NULL);

    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, CHUBBY_GET_LEASE_REPLY_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_get_lease_reply, NULL);
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, SUCCESS_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_leader, NULL);
    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, TIMEUP_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_get_lease, NULL);


    gpcc_fsm_put(LMA_LEAD_CMDSTATE, CHUBBY_PUT_REPLY_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_leader_put_reply, NULL);
    gpcc_fsm_put(LMA_LEAD_CMDSTATE, CHUBBY_GET_REPLY_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_leader_get_master_reply, NULL);
    gpcc_fsm_put(LMA_LEAD_CMDSTATE, CHUBBY_RENEW_REPLY_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_leader_renew_reply, NULL);
    gpcc_fsm_put(LMA_LEAD_CMDSTATE, FAILURE_EVENT, LMA_LEADLESS_CMDSTATE,
		 a_lma_leader_failure, NULL);
    gpcc_fsm_put(LMA_LEAD_CMDSTATE, TIMEUP_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_leader_renew, NULL);


    gpcc_fsm_put(LMA_LEADLESS_CMDSTATE, FAILURE_EVENT, LMA_STANDBY_CMDSTATE,
		 a_lma_standby, NULL);
    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, TIMEUP_EVENT, LMA_STANDBY_CMDSTATE,
		 a_lma_standby_get_lease, NULL);
    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, CHUBBY_GET_LEASE_REPLY_EVENT, LMA_STANDBY_CMDSTATE,
		 a_lma_standby_get_lease_reply, NULL);
    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, CHUBBY_GET_REPLY_EVENT, LMA_STANDBY_CMDSTATE,
		 a_lma_standby_get_reply, NULL);
    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, SUCCESS_EVENT, LMA_LEAD_CMDSTATE,
		 a_lma_leader, NULL);
    gpcc_fsm_put(LMA_STANDBY_CMDSTATE, FAILURE_EVENT, LMA_STANDBY_CMDSTATE,
		 a_lma_standby, NULL);


    /* ha set */
    gpcc_fsm_put(HASET_OFF_CMDSTATE, INIT_EVENT, HASET_WAIT_CMDSTATE, 
		 a_haset_init, NULL);
    gpcc_fsm_put(HASET_WAIT_CMDSTATE, NET_OPENED_EVENT, HASET_WAIT_CMDSTATE, 
		 a_haset_opened, NULL);
    gpcc_fsm_put(HASET_WAIT_CMDSTATE, NET_CLOSED_EVENT, HASET_WAIT_CMDSTATE, 
		 a_haset_closed, NULL);
    gpcc_fsm_put(HASET_WAIT_CMDSTATE, FAILURE_EVENT, HASET_WAIT_CMDSTATE, NULL);
    gpcc_fsm_put(HASET_WAIT_CMDSTATE, SUCCESS_EVENT, HASET_READY_CMDSTATE, 
		 a_haset_ready, NULL);

    gpcc_fsm_put(HASET_READY_CMDSTATE, NET_OPENED_EVENT, HASET_READY_CMDSTATE, 
		 a_haset_opened, NULL);
    gpcc_fsm_put(HASET_READY_CMDSTATE, NET_CLOSED_EVENT, HASET_READY_CMDSTATE, 
		 a_haset_closed, NULL);
    gpcc_fsm_put(HASET_READY_CMDSTATE, FAILURE_EVENT, HASET_WAIT_CMDSTATE, 
		 a_haset_failure, NULL);
    gpcc_fsm_put(HASET_READY_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(HASET_WAIT_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(HASET_OFF_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);


    /* fts */
    gpcc_fsm_put(FTS_CMDSTATE, INIT_EVENT, FTS_CMDSTATE,
		 a_fts_init, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_ABSENT_EVENT, FTS_EMPTY_CMDSTATE, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_PRESENT_EVENT, FTS_CMDSTATE,
		 a_fts_setup_and_gethostname, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, SSH_HOSTNAME_REPLY_EVENT, FTS_CMDSTATE,
		 a_fts_gethostname_reply, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, SSH_ADDRESS_REPLY_EVENT, FTS_CMDSTATE,    
		 a_fts_getaddress_reply, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, NODE_START_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_kick_off, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, FAILURE_EVENT, FTS_CMDSTATE,
		 a_fts_init_failure, NULL);

    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_STATUS_EVENT, FTS_CMDSTATE,
		 a_fts_array_status_reply, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_STOP_EVENT, FTS_CMDSTATE,
		 a_fts_waiting, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_INIT_EVENT, FTS_CMDSTATE,
		 a_fts_waiting, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_START_EVENT, FTS_CMDSTATE,
		 a_fts_waiting, NULL);
    gpcc_fsm_put(FTS_CMDSTATE, ARRAY_DELETE_EVENT, FTS_CMDSTATE,
		 a_fts_waiting, NULL);

    /* fts break event */
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, BREAK_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_break, NULL);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, BREAK_EVENT, FTS_WAIT_CMDSTATE, 
		 a_fts_break, NULL);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, BREAK_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_break, NULL);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, BREAK_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_break, NULL);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, BREAK_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_break, NULL);
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, BREAK_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_break, NULL);

    /* fts wait */
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_STATUS_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_array_status_reply, NULL);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_STOP_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_waiting, NULL);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_INIT_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_waiting, NULL);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_START_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_waiting, NULL);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_DELETE_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_waiting, NULL);

    gpcc_fsm_put(FTS_WAIT_CMDSTATE, CONTINUE_EVENT, FTS_WAIT_CMDSTATE,
		 a_fts_array_present, NULL);
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_ABSENT_EVENT, FTS_EMPTY_CMDSTATE, NULL);

    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_INIT_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_array_init_start_nodes, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, ARRAY_PRESENT_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_setup_and_gethostname, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, SSH_HOSTNAME_REPLY_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_gethostname_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, SSH_ADDRESS_REPLY_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_getaddress_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, NODE_START_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_kick_off, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, FAILURE_EVENT, FTS_EMPTY_CMDSTATE, 0,
		 a_fts_array_init_failure, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, TIMEUP_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_array_init_start_nodes_failure, a_fts_array_init_failure, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, CONTINUE_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_init, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, ARRAY_STATUS_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_array_status_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, ARRAY_INIT_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_busy_ok_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, ARRAY_STOP_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, ARRAY_START_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING0_CMDSTATE, ARRAY_DELETE_EVENT, FTS_INITIALIZING0_CMDSTATE,
		 a_fts_busy_reply, NULL);

    /* fts empty */
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_START_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_STOP_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_DELETE_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_EMPTY_CMDSTATE, ARRAY_STATUS_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_array_status_reply, NULL);

    /* fts initializing */
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_init_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, SUCCESS_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_array_init_success, a_fts_create_segment_slot, NULL);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, FAILURE_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_array_init_failure, NULL);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_INIT_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_busy_ok_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_STOP_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_START_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_DELETE_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_INITIALIZING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_INITIALIZING_CMDSTATE,
		 a_fts_array_status_reply, NULL);
    
    /* fts stopped */
    gpcc_fsm_put(FTS_WAIT_CMDSTATE, ARRAY_PRESENT_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_create_segment_slot, a_fts_array_stop_forsure, NULL);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_START_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_array_start, NULL);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_STOP_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_busy_ok_reply, NULL);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_INIT_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_DELETE_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, ARRAY_STATUS_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_array_status_reply, NULL);    

    /* fts stopped - segment command */
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, SEGMENT_START_EVENT, FTS_STOPPED_BUSY_CMDSTATE,
		 a_fts_segment_start, NULL);
    gpcc_fsm_put(FTS_STOPPED_CMDSTATE, SEGMENT_STOP_EVENT, FTS_STOPPED_BUSY_CMDSTATE,
		 a_fts_segment_stop, NULL);
    gpcc_fsm_put(FTS_STOPPED_BUSY_CMDSTATE, CONTINUE_EVENT, FTS_STOPPED_CMDSTATE, 
		 a_fts_segment_command_reply, NULL);


    /* fts starting */
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_START_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_ok_reply, NULL);    
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_STOP_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_reply, NULL);    
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_INIT_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_DELETE_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_array_status_reply, NULL);    
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_array_start_reply, NULL);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, SUCCESS_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_running, NULL);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, FAILURE_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_failure, NULL);

    /* fts starting - reject segment command */
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, SEGMENT_START_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STARTING_CMDSTATE, SEGMENT_STOP_EVENT, FTS_STARTING_CMDSTATE,
		 a_fts_busy_reply, NULL);

    /* fts running */
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_DELETE_EVENT, FTS_DELETING_CMDSTATE,
		 a_fts_array_delete, NULL);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_STOP_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_stop, NULL);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_array_status_reply, NULL);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_START_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_busy_ok_reply, NULL);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, ARRAY_INIT_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_busy_reply, NULL);

    /* fts running - segment command */
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, SEGMENT_START_EVENT, FTS_RUNNING_BUSY_CMDSTATE,
		 a_fts_segment_start, NULL);
    gpcc_fsm_put(FTS_RUNNING_CMDSTATE, SEGMENT_STOP_EVENT, FTS_RUNNING_BUSY_CMDSTATE,
		 a_fts_segment_stop, NULL);
    gpcc_fsm_put(FTS_RUNNING_BUSY_CMDSTATE, CONTINUE_EVENT, FTS_RUNNING_CMDSTATE, NULL);

    /* fts stopping */
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_START_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_reply, NULL);    
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_STOP_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_ok_reply, NULL);  
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_INIT_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_DELETE_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_status_reply, NULL);    
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_array_stop_reply, NULL);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, SUCCESS_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_stopped, NULL);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, FAILURE_EVENT, FTS_STOPPED_CMDSTATE,
		 a_fts_failure, NULL);

    /* fts starting - reject segment command */
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, SEGMENT_START_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_STOPPING_CMDSTATE, SEGMENT_STOP_EVENT, FTS_STOPPING_CMDSTATE,
		 a_fts_busy_reply, NULL);

    /* fts deleting */
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, ARRAY_START_EVENT, FTS_DELETING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, ARRAY_STOP_EVENT, FTS_DELETING_CMDSTATE,
		 a_fts_busy_reply, NULL);
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, ARRAY_INIT_EVENT, FTS_DELETING_CMDSTATE,
		 a_fts_busy_reply, NULL);    
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, ARRAY_DELETE_EVENT, FTS_DELETING_CMDSTATE,
		 a_fts_busy_ok_reply, NULL);
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, ARRAY_STATUS_EVENT, FTS_DELETING_CMDSTATE,
		 a_fts_array_status_reply, NULL);
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, ARRAY_REPLY_MSG_EVENT, FTS_DELETING_CMDSTATE,
		 a_fts_array_delete_reply, NULL);
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, SUCCESS_EVENT, FTS_EMPTY_CMDSTATE,
		 a_fts_deleted, a_fts_array_delete_shutdown_nodes, NULL);
    gpcc_fsm_put(FTS_DELETING_CMDSTATE, FAILURE_EVENT, FTS_RUNNING_CMDSTATE,
		 a_fts_failure, NULL);


    /* init */
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, INIT_EVENT, FTS_INITDB_CMDSTATE,
		 a_initdb_init, a_initdb_do, NULL);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_INITDB_CMDSTATE,
		 a_initdb_reply, NULL);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, CONTINUE_EVENT, FTS_INITDB_CMDSTATE,
		 a_initdb_do, NULL);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_initdb_success, NULL);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_initdb_failure, NULL);
    gpcc_fsm_put(FTS_INITDB_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_initdb_timeout, NULL);

    /* start */
    gpcc_fsm_put(FTS_START_CMDSTATE, INIT_EVENT, FTS_START_CMDSTATE,
		 a_start_do, NULL);
    gpcc_fsm_put(FTS_START_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_START_CMDSTATE,
		 a_start_reply, NULL);
    gpcc_fsm_put(FTS_START_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_start_success, NULL);
    gpcc_fsm_put(FTS_START_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_start_failure, NULL);
    gpcc_fsm_put(FTS_START_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_start_timeout, NULL);

    /* stop */
    
    gpcc_fsm_put(FTS_STOP_CMDSTATE, INIT_EVENT, FTS_STOP_CMDSTATE,
		 a_stop_do, NULL);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_STOP_CMDSTATE,
		 a_stop_reply, NULL);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_stop_success, NULL);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_stop_failure, NULL);
    gpcc_fsm_put(FTS_STOP_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_stop_timeout, NULL);

 
    /* delete */
    
    gpcc_fsm_put(FTS_DELETE_CMDSTATE, INIT_EVENT, FTS_DELETE_CMDSTATE,
		 a_delete_do, NULL);
    gpcc_fsm_put(FTS_DELETE_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, FTS_DELETE_CMDSTATE,
		 a_delete_reply, NULL);
    gpcc_fsm_put(FTS_DELETE_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_delete_success, NULL);
    gpcc_fsm_put(FTS_DELETE_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_delete_failure, NULL);
    gpcc_fsm_put(FTS_DELETE_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_delete_timeout, NULL);


   /* status */
    
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, INIT_EVENT, FTS_STATUS_CMDSTATE,
		 a_status_do, NULL);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, STATUS_REPLY_MSG_EVENT, FTS_STATUS_CMDSTATE,
		 a_status_reply, NULL);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_status_success, NULL);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_status_failure, NULL);
    gpcc_fsm_put(FTS_STATUS_CMDSTATE, TIMEUP_EVENT, NULL_CMDSTATE,
		 a_status_timeout, NULL);

    /* segment start */
    gpcc_fsm_put(FTS_SEGMENT_START_CMDSTATE, INIT_EVENT, FTS_SEGMENT_START_WAIT_CMDSTATE,
		 a_segment_start_do, NULL);
    gpcc_fsm_put(FTS_SEGMENT_START_WAIT_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_SEGMENT_START_WAIT_CMDSTATE,
		 a_segment_start_units_start_reply, NULL);
    gpcc_fsm_put(FTS_SEGMENT_START_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_segment_start_success, NULL);
    gpcc_fsm_put(FTS_SEGMENT_START_WAIT_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_segment_start_failure, NULL);

    /* segment stop */
    gpcc_fsm_put(FTS_SEGMENT_STOP_CMDSTATE, INIT_EVENT, FTS_SEGMENT_STOP_WAIT_CMDSTATE,
		 a_segment_stop_do, NULL);
    gpcc_fsm_put(FTS_SEGMENT_STOP_WAIT_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_SEGMENT_STOP_WAIT_CMDSTATE,
		 a_segment_stop_units_stop_reply, NULL);
    gpcc_fsm_put(FTS_SEGMENT_STOP_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_segment_stop_success, NULL);
    gpcc_fsm_put(FTS_SEGMENT_STOP_WAIT_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_segment_stop_failure, NULL);

    /* array stop */
    gpcc_fsm_put(FTS_ARRAY_STOP_CMDSTATE, INIT_EVENT, FTS_ARRAY_STOP_WAIT_CMDSTATE,
		 a_array_stop_do, NULL);
    gpcc_fsm_put(FTS_ARRAY_STOP_WAIT_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_ARRAY_STOP_WAIT_CMDSTATE,
		 a_array_stop_units_stop_reply, NULL);
    gpcc_fsm_put(FTS_ARRAY_STOP_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_array_stop_success, NULL);
    gpcc_fsm_put(FTS_ARRAY_STOP_WAIT_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_array_stop_failure, NULL);

    /* array delete */
    gpcc_fsm_put(FTS_ARRAY_DELETE_CMDSTATE, INIT_EVENT, FTS_ARRAY_DELETE_WAIT_CMDSTATE,
		 a_array_delete_do, NULL);
    gpcc_fsm_put(FTS_ARRAY_DELETE_WAIT_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_ARRAY_DELETE_WAIT_CMDSTATE,
		 a_array_delete_units_delete_reply, NULL);
    gpcc_fsm_put(FTS_ARRAY_DELETE_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_array_delete_success, NULL);
    gpcc_fsm_put(FTS_ARRAY_DELETE_WAIT_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_array_delete_failure, NULL);

    /* array initdb */
    gpcc_fsm_put(FTS_ARRAY_INIT_CMDSTATE, INIT_EVENT, FTS_ARRAY_INIT_WAIT_CMDSTATE,
		 a_array_init_do, NULL);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_ARRAY_INIT_WAIT_CMDSTATE,
		 a_array_init_units_init_reply, NULL);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_array_init_success, NULL);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_array_init_failure, NULL);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_CMDSTATE, CONTINUE_EVENT, FTS_ARRAY_INIT_WAIT_SYNC_STANDBY_QD_CMDSTATE,
		 a_array_sync_standby_qd, NULL);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_SYNC_STANDBY_QD_CMDSTATE, TIMEUP_EVENT, FTS_ARRAY_INIT_WAIT_SYNC_STANDBY_QD_CMDSTATE,
		 a_array_wait_sync_standby_qd, NULL);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_SYNC_STANDBY_QD_CMDSTATE, SUCCESS_EVENT, NULL_EVENT,
		 a_array_init_success, NULL);
    gpcc_fsm_put(FTS_ARRAY_INIT_WAIT_SYNC_STANDBY_QD_CMDSTATE, FAILURE_EVENT, NULL_EVENT,
		 a_array_init_failure, NULL);
		 

    /* array start */
    /* start qd in utility mode */
    gpcc_fsm_put(FTS_ARRAY_START_CMDSTATE, INIT_EVENT, FTS_START_UTILITY_QD_CMDSTATE,
		 a_array_start_utility_qd, NULL);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_UTILITY_QD_CMDSTATE,
		 a_array_start_units_start_reply, NULL);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, SUCCESS_EVENT, FTS_START_GPCONF_CMDSTATE,
		 a_array_start_gpconf, NULL);
    gpcc_fsm_put(FTS_START_UTILITY_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, NULL);

    /* update gp_configuration */
    gpcc_fsm_put(FTS_START_GPCONF_CMDSTATE, SUCCESS_EVENT, FTS_START_UTILITY_QD_STOP_CMDSTATE,
		 a_array_start_utility_qd_stop, NULL);
    gpcc_fsm_put(FTS_START_GPCONF_CMDSTATE, FAILURE_EVENT, FTS_START_GPCONF_FAILURE_CMDSTATE,
		 a_array_start_utility_qd_stop, NULL);

    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_GPCONF_FAILURE_CMDSTATE,
		 a_array_start_units_stop_reply, NULL);
    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, NULL);
    gpcc_fsm_put(FTS_START_GPCONF_FAILURE_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, NULL);

    /* stop qd */
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_UTILITY_QD_STOP_CMDSTATE,
		 a_array_start_units_stop_reply, NULL);
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, SUCCESS_EVENT, FTS_START_QES_CMDSTATE,
		 a_array_start_qes, NULL);
    gpcc_fsm_put(FTS_START_UTILITY_QD_STOP_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, NULL);

    /* start all qes */
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_QES_CMDSTATE,
		 a_array_start_units_start_reply, NULL);
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, SUCCESS_EVENT, FTS_START_QD_CMDSTATE,
		 a_array_start_qd, NULL);
    gpcc_fsm_put(FTS_START_QES_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, NULL);
		 
    /* start qd */
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_QD_CMDSTATE,
		 a_array_start_units_start_reply, NULL);
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, SUCCESS_EVENT, FTS_START_STANDBY_QD_CMDSTATE,
		 a_array_start_standby_qd, NULL);
    gpcc_fsm_put(FTS_START_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, NULL);

    /* start standby qd */
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, UNITS_REPLY_MSG_EVENT, FTS_START_STANDBY_QD_CMDSTATE,
		 a_array_start_units_start_reply, NULL);
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_array_start_success, a_array_start_cleanup, NULL);
    gpcc_fsm_put(FTS_START_STANDBY_QD_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_array_start_failure, a_array_start_cleanup, NULL);



    /* segment unit */
    gpcc_fsm_put(UNIT_CMDSTATE, UNIT_STOP_EVENT, UNIT_STOPPING_CMDSTATE,
		 a_unit_stop, NULL);
    gpcc_fsm_put(UNIT_CMDSTATE, UNIT_START_EVENT, UNIT_STARTING_CMDSTATE,
		 a_unit_start, NULL);

    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, UNIT_STOP_EVENT, UNIT_STOPPING_CMDSTATE,
		 a_unit_busy_ok_reply, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, UNIT_START_EVENT, UNIT_STOPPING_CMDSTATE,
		 a_unit_busy_error_reply, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, UNIT_STOPPING_CMDSTATE,
		 a_unit_stop_reply, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, TIMEUP_EVENT, UNIT_INVALID_CMDSTATE,
		 a_unit_timeout, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, SUCCESS_EVENT, UNIT_STOPPED_CMDSTATE,
		 a_unit_stopped, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, FAILURE_EVENT, UNIT_INVALID_CMDSTATE,
		 a_unit_invalid, NULL);
    gpcc_fsm_put(UNIT_STOPPED_CMDSTATE, UNIT_START_EVENT, UNIT_STARTING_CMDSTATE,
		 a_unit_start, NULL);
    gpcc_fsm_put(UNIT_STOPPED_CMDSTATE, UNIT_STOP_EVENT, UNIT_STOPPED_CMDSTATE,
		 a_unit_busy_ok_reply, NULL);

    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, UNIT_STARTING_CMDSTATE,
		 a_unit_start_reply, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, TIMEUP_EVENT, UNIT_INVALID_CMDSTATE,
		 a_unit_timeout, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, SUCCESS_EVENT, UNIT_RUNNING_CMDSTATE,
		 a_unit_running, a_unit_schedule_check, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, FAILURE_EVENT, UNIT_INVALID_CMDSTATE,
		 a_unit_invalid, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, UNIT_START_EVENT, UNIT_STARTING_CMDSTATE,
		 a_unit_busy_ok_reply, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, UNIT_STOP_EVENT, UNIT_STARTING_CMDSTATE,
		 a_unit_busy_error_reply, NULL);
 
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, UNIT_STOP_EVENT, UNIT_STOPPING_CMDSTATE,
		 a_unit_stop, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, UNIT_START_EVENT, UNIT_RUNNING_CMDSTATE,
		 a_unit_busy_ok_reply, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, TIMEUP_EVENT, UNIT_RUNNING_CMDSTATE,
		 a_unit_postgres_status, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, STATUS_REPLY_MSG_EVENT, UNIT_RUNNING_CMDSTATE,
		 a_unit_postgres_status_reply, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, SUCCESS_EVENT, UNIT_RUNNING_CMDSTATE,
		 a_unit_schedule_check, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, FAILURE_EVENT, UNIT_INVALID_CMDSTATE,
		 a_unit_failover, NULL);

		 
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, UNIT_DELETE_EVENT, UNIT_STARTING_CMDSTATE,
		 a_unit_busy_error_reply, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, UNIT_DELETE_EVENT, UNIT_STOPPING_CMDSTATE,
		 a_unit_busy_error_reply, NULL);
    gpcc_fsm_put(UNIT_STOPPED_CMDSTATE, UNIT_DELETE_EVENT, UNIT_STOPPED_CMDSTATE,
		 a_unit_busy_error_reply, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, UNIT_DELETE_EVENT, UNIT_DELETING_CMDSTATE,
		 a_unit_delete, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, UNIT_DELETE_EVENT, UNIT_DELETING_CMDSTATE,
		 a_unit_busy_ok_reply, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, SEGMENT_REPLY_MSG_EVENT, UNIT_DELETING_CMDSTATE,
		 a_unit_delete_reply, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, TIMEUP_EVENT, UNIT_INVALID_CMDSTATE,
		 a_unit_timeout, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, SUCCESS_EVENT, UNIT_DELETED_CMDSTATE,
		 NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, FAILURE_EVENT, UNIT_RUNNING_CMDSTATE,
		 NULL);


    /* unit break event */
    gpcc_fsm_put(UNIT_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STOPPED_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_DELETED_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_INVALID_CMDSTATE, BREAK_EVENT, UNIT_CMDSTATE, NULL);

    /* invalid */
    gpcc_fsm_put(UNIT_INVALID_CMDSTATE, UNIT_STOP_EVENT, UNIT_INVALID_CMDSTATE, 
		 a_unit_invalid, NULL);
    gpcc_fsm_put(UNIT_INVALID_CMDSTATE, UNIT_START_EVENT, UNIT_INVALID_CMDSTATE, 
		 a_unit_invalid, NULL);
    gpcc_fsm_put(UNIT_INVALID_CMDSTATE, UNIT_DELETE_EVENT, UNIT_DELETING_CMDSTATE,
		 a_unit_delete_invalid, NULL);
    

    /* shutdown */
    gpcc_fsm_put(UNIT_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STOPPED_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_DELETED_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_INVALID_CMDSTATE, SHUTDOWN_EVENT, NULL_CMDSTATE, NULL);

    /* invalid */
    gpcc_fsm_put(UNIT_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STOPPED_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_DELETED_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    gpcc_fsm_put(UNIT_INVALID_CMDSTATE, INVALID_EVENT, UNIT_INVALID_CMDSTATE, NULL);
    
    /* unit status */
    gpcc_fsm_put(UNIT_CMDSTATE, UNIT_STATUS_EVENT, UNIT_CMDSTATE, 
		 a_unit_status, NULL);
    gpcc_fsm_put(UNIT_STOPPING_CMDSTATE, UNIT_STATUS_EVENT, UNIT_STOPPING_CMDSTATE,
		 a_unit_status, NULL);
    gpcc_fsm_put(UNIT_STOPPED_CMDSTATE, UNIT_STATUS_EVENT, UNIT_STOPPED_CMDSTATE,
		 a_unit_status, NULL);
    gpcc_fsm_put(UNIT_STARTING_CMDSTATE, UNIT_STATUS_EVENT, UNIT_STARTING_CMDSTATE,
		 a_unit_status, NULL);
    gpcc_fsm_put(UNIT_RUNNING_CMDSTATE, UNIT_STATUS_EVENT, UNIT_RUNNING_CMDSTATE,
		 a_unit_status, NULL);
    gpcc_fsm_put(UNIT_DELETING_CMDSTATE, UNIT_STATUS_EVENT, UNIT_DELETING_CMDSTATE,
		 a_unit_status, NULL);
    gpcc_fsm_put(UNIT_DELETED_CMDSTATE, UNIT_STATUS_EVENT, UNIT_DELETED_CMDSTATE,
		 a_unit_status, NULL);
    gpcc_fsm_put(UNIT_INVALID_CMDSTATE, UNIT_STATUS_EVENT, UNIT_INVALID_CMDSTATE,
		 a_unit_status, NULL);

    /* user manager */
    gpcc_fsm_put(USER_CMDSTATE, USER_LOGIN_EVENT, USER_CMDSTATE,
		 a_user_login, NULL);
    gpcc_fsm_put(USER_CMDSTATE, USER_LOGOUT_EVENT, USER_CMDSTATE,
		 a_user_logout, NULL);
    gpcc_fsm_put(USER_CMDSTATE, TIMEUP_EVENT, USER_CMDSTATE,
		 a_user_check_expire, NULL);


    /* login */
    gpcc_fsm_put(LOGIN_CMDSTATE, USER_LOGIN_EVENT, LOGIN_LOGGING_CMDSTATE,
		 a_login_get_password_file, NULL);
    gpcc_fsm_put(LOGIN_LOGGING_CMDSTATE, CHUBBY_GET_REPLY_EVENT, LOGIN_LOGGING_CMDSTATE,
		 a_login_logging_on, NULL);
    gpcc_fsm_put(LOGIN_LOGGING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_login_failure, a_login_finish, NULL);
    gpcc_fsm_put(LOGIN_LOGGING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_login_logged_on, a_login_finish, NULL);
    gpcc_fsm_put(LOGIN_LOGGING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_login_failure, a_login_finish, NULL);

    /* admin manager */
    gpcc_fsm_put(ADMIN_CMDSTATE, ADMIN_ADD_EVENT, ADMIN_CMDSTATE,
		 a_admin_forward, NULL);
    gpcc_fsm_put(ADMIN_CMDSTATE, ADMIN_DELETE_EVENT, ADMIN_CMDSTATE,
		 a_admin_forward, NULL);
    gpcc_fsm_put(ADMIN_CMDSTATE, ADMIN_LIST_EVENT, ADMIN_CMDSTATE,
		 a_admin_forward, NULL);
    gpcc_fsm_put(ADMIN_CMDSTATE, ADMIN_PASSWORD_EVENT, ADMIN_CMDSTATE,
		 a_admin_forward, NULL);
    
    /* admin add */    
    gpcc_fsm_put(ADMIN_ADD_CMDSTATE, ADMIN_ADD_EVENT, ADMIN_ADDING_CMDSTATE,
		 a_admin_add_init, NULL);
    gpcc_fsm_put(ADMIN_ADDING_CMDSTATE, CONTINUE_EVENT, ADMIN_ADDING_CMDSTATE,
		 a_admin_get_password_file_lease, NULL);
    gpcc_fsm_put(ADMIN_ADDING_CMDSTATE, CHUBBY_GET_LEASE_REPLY_EVENT, ADMIN_ADDING_CMDSTATE,
		 a_admin_get_password_file, NULL);
    gpcc_fsm_put(ADMIN_ADDING_CMDSTATE, CHUBBY_GET_REPLY_EVENT, ADMIN_ADDING_CMDSTATE,
		 a_admin_add_set_password_file, NULL);
    gpcc_fsm_put(ADMIN_ADDING_CMDSTATE, CHUBBY_PUT_REPLY_EVENT, ADMIN_ADDING_CMDSTATE,
		 a_admin_giveup_lease, NULL);
    gpcc_fsm_put(ADMIN_ADDING_CMDSTATE, CHUBBY_GIVEUP_LEASE_REPLY_EVENT, ADMIN_ADDING_CMDSTATE,
		 a_admin_add_done, NULL);
    gpcc_fsm_put(ADMIN_ADDING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_admin_add_success, NULL);
    gpcc_fsm_put(ADMIN_ADDING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_admin_add_failure, NULL);

    /* admin delete */
    gpcc_fsm_put(ADMIN_DELETE_CMDSTATE, ADMIN_DELETE_EVENT, ADMIN_DELETING_CMDSTATE,
		 a_admin_delete_init, NULL);
    gpcc_fsm_put(ADMIN_DELETING_CMDSTATE, CONTINUE_EVENT, ADMIN_DELETING_CMDSTATE,
		 a_admin_get_password_file_lease, NULL);
    gpcc_fsm_put(ADMIN_DELETING_CMDSTATE, CHUBBY_GET_LEASE_REPLY_EVENT, ADMIN_DELETING_CMDSTATE,
		 a_admin_get_password_file, NULL);
    gpcc_fsm_put(ADMIN_DELETING_CMDSTATE, CHUBBY_GET_REPLY_EVENT, ADMIN_DELETING_CMDSTATE,
		 a_admin_delete_set_password_file, NULL);
    gpcc_fsm_put(ADMIN_DELETING_CMDSTATE, CHUBBY_PUT_REPLY_EVENT, ADMIN_DELETING_CMDSTATE,
		 a_admin_giveup_lease, NULL);
    gpcc_fsm_put(ADMIN_DELETING_CMDSTATE, CHUBBY_GIVEUP_LEASE_REPLY_EVENT, ADMIN_DELETING_CMDSTATE,
		 a_admin_delete_done, NULL);
    gpcc_fsm_put(ADMIN_DELETING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_admin_delete_success, NULL);
    gpcc_fsm_put(ADMIN_DELETING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_admin_delete_failure, NULL);


    /* admin list */
    gpcc_fsm_put(ADMIN_LIST_CMDSTATE, ADMIN_LIST_EVENT, ADMIN_LISTING_CMDSTATE,
		 a_admin_list_init, NULL);
    gpcc_fsm_put(ADMIN_LISTING_CMDSTATE, CONTINUE_EVENT, ADMIN_LISTING_CMDSTATE,
		 a_admin_get_password_file_direct, NULL);
    gpcc_fsm_put(ADMIN_LISTING_CMDSTATE, CHUBBY_GET_REPLY_EVENT, ADMIN_LISTING_CMDSTATE,
		 a_admin_list_done, NULL);
    gpcc_fsm_put(ADMIN_LISTING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_admin_list_success, NULL);
    gpcc_fsm_put(ADMIN_LISTING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_admin_list_failure, NULL);


    /* admin password */
    gpcc_fsm_put(ADMIN_PASSWORD_CMDSTATE, ADMIN_PASSWORD_EVENT, ADMIN_PASSWORD_CHANGING_CMDSTATE,
		 a_admin_password_init, NULL);
    gpcc_fsm_put(ADMIN_PASSWORD_CHANGING_CMDSTATE, CONTINUE_EVENT, ADMIN_PASSWORD_CHANGING_CMDSTATE,
		 a_admin_get_password_file_lease, NULL);
    gpcc_fsm_put(ADMIN_PASSWORD_CHANGING_CMDSTATE, CHUBBY_GET_LEASE_REPLY_EVENT, ADMIN_PASSWORD_CHANGING_CMDSTATE,
		 a_admin_get_password_file, NULL);
    gpcc_fsm_put(ADMIN_PASSWORD_CHANGING_CMDSTATE, CHUBBY_GET_REPLY_EVENT, ADMIN_PASSWORD_CHANGING_CMDSTATE,
		 a_admin_password_set_password_file, NULL);
    gpcc_fsm_put(ADMIN_PASSWORD_CHANGING_CMDSTATE, CHUBBY_PUT_REPLY_EVENT, ADMIN_PASSWORD_CHANGING_CMDSTATE,
		 a_admin_giveup_lease, NULL);
    gpcc_fsm_put(ADMIN_PASSWORD_CHANGING_CMDSTATE, CHUBBY_GIVEUP_LEASE_REPLY_EVENT, ADMIN_PASSWORD_CHANGING_CMDSTATE,
		 a_admin_password_done, NULL);
    gpcc_fsm_put(ADMIN_PASSWORD_CHANGING_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_admin_password_success, NULL);
    gpcc_fsm_put(ADMIN_PASSWORD_CHANGING_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_admin_password_failure, NULL);

    
    /* ssh gethostname */
    gpcc_fsm_put(SSH_HOSTNAME_CMDSTATE, INIT_EVENT, SSH_HOSTNAME_WAIT_CMDSTATE,
		 a_ssh_gethostname, NULL);
    gpcc_fsm_put(SSH_HOSTNAME_WAIT_CMDSTATE, TIMEUP_EVENT, SSH_HOSTNAME_WAIT_CMDSTATE,
		 a_ssh_gethostname_wait, NULL);
    gpcc_fsm_put(SSH_HOSTNAME_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_ssh_gethostname_success, NULL);
    gpcc_fsm_put(SSH_HOSTNAME_WAIT_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_ssh_gethostname_failure, NULL);
    gpcc_fsm_put(SSH_HOSTNAME_WAIT_CMDSTATE, CONTINUE_EVENT, SSH_HOSTNAME_WAIT_CMDSTATE,
		 a_ssh_gethostname, NULL);


    /* ssh getaddress */
    gpcc_fsm_put(SSH_ADDRESS_CMDSTATE, INIT_EVENT, SSH_ADDRESS_WAIT_CMDSTATE,
		 a_ssh_getaddress, NULL);
    gpcc_fsm_put(SSH_ADDRESS_WAIT_CMDSTATE, TIMEUP_EVENT, SSH_ADDRESS_WAIT_CMDSTATE,
		 a_ssh_getaddress_wait, NULL);
    gpcc_fsm_put(SSH_ADDRESS_WAIT_CMDSTATE, SUCCESS_EVENT, NULL_CMDSTATE,
		 a_ssh_getaddress_success, NULL);
    gpcc_fsm_put(SSH_ADDRESS_WAIT_CMDSTATE, FAILURE_EVENT, NULL_CMDSTATE,
		 a_ssh_getaddress_failure, NULL);
    gpcc_fsm_put(SSH_ADDRESS_WAIT_CMDSTATE, CONTINUE_EVENT, SSH_ADDRESS_WAIT_CMDSTATE,
		 a_ssh_getaddress, NULL);

}
