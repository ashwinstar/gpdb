#ifndef ENUM_H
#define ENUM_H

typedef enum msg_t msg_t;
enum msg_t {
    NULL_MSG,
    SEGMENT_REQUEST_MSG = 200,
    SEGMENT_REPLY_MSG,
    HOST_REQUEST_MSG,
    ARRAY_REQUEST_MSG,
    ARRAY_REPLY_MSG,
    STATUS_REQUEST_MSG,
    STATUS_REPLY_MSG,
    ALERT_MSG = 400,
    ERROR_MSG,

    MSG_MAX,
};

extern const char* msg_str(msg_t msg);


typedef enum role_t role_t;
enum role_t {
    NULL_ROLE,
    MA_ROLE,
    HA_ROLE,
    QD_ROLE,
    QE_ROLE,

    ROLE_MAX,
};

extern const char* role_str(role_t role);

typedef enum event_t event_t;
enum event_t {
    NULL_EVENT,
    TICK_EVENT,
    TIMEUP_EVENT,
    INIT_EVENT,
    SUCCESS_EVENT,
    FAILURE_EVENT,
    BREAK_EVENT,

    NET_OPENED_EVENT,
    NET_CLOSED_EVENT,
    NET_RECEIVED_EVENT,

    HTTP_REQUEST_EVENT,

    SEG_INITDB_EVENT,
    SEG_INITDB_NEXT_EVENT,
    SEG_INITDB_DONE_EVENT,
    SEG_INITDB_FAIL0_EVENT,
    SEG_INITDB_FAIL1_EVENT,
    SEG_INITDB_FAIL2_EVENT,
    SEG_INITDB_REPLY_EVENT,

    SEG_START_EVENT,
    SEG_START_REPLY_EVENT,

    SEG_STOP_EVENT,
    SEG_STOP_REPLY_EVENT,

    SEG_STATUS_EVENT,
    SEG_STATUS_REPLY_EVENT,


    /* segment msg */
    SEGMENT_REQUEST_MSG_EVENT = 200,
    SEGMENT_REPLY_MSG_EVENT,
    HOST_REQUEST_MSG_EVENT,
    ARRAY_REQUEST_MSG_EVENT,
    ARRAY_REPLY_MSG_EVENT,
    STATUS_REQUEST_MSG_EVENT,
    STATUS_REPLY_MSG_EVENT,
    ALERT_MSG_EVENT = 400,
    ERROR_MSG_EVENT,


    
    
    
    EVENT_MAX,
};

extern const char* event_str(event_t event);

typedef enum cmdstate_t cmdstate_t;
enum cmdstate_t {
    NULL_CMDSTATE,
    SYSTEM_CMDSTATE,

    MA_CMDSTATE,

    HTTP_REQUEST_CMDSTATE,

    SEG_INIT_CMDSTATE,
    SEG_INITDB_CMDSTATE,
    SEG_INITDB_START_CMDSTATE,
    SEG_INITDB_CHECK_CMDSTATE,
    SEG_STARTING_CMDSTATE,
    SEG_RUNNING_CMDSTATE,
    SEG_STOPPING_CMDSTATE,
    SEG_STOPPED_CMDSTATE,
    

    CMDSTATE_MAX,
};

extern const char* cmdstate_str(cmdstate_t cmdstate);


#endif /* ENUM_H */
