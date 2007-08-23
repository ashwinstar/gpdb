#ifndef ENUM_H
#define ENUM_H

typedef enum msg_t msg_t;
enum msg_t {
    NULL_MSG,
    GET_IDENTITY_MSG,
    IDENTITY_MSG,
    KEEP_ALIVE_MSG,
    VOTE_ME_MSG = 10,
    VOTE_REPLY_MSG,
    CSN_GET_MSG = 20,
    CSN_REPLY_MSG,
    CONFIG_GET_MSG,
    CONFIG_REPLY_MSG,
    LEADMA_GET_MSG = 30,
    LEADMA_REPLY_MSG,
    SEGMENT_REQUEST_MSG = 100,
    SEGMENT_REPLY_MSG,
    HOST_REQUEST_MSG,
    ARRAY_REQUEST_MSG,
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

    GET_IDENTITY_MSG_EVENT,
    IDENTITY_MSG_EVENT,

    HTTP_REQUEST_EVENT,

    SEG_INITDB_EVENT,
    SEG_INITDB_NEXT_EVENT,
    SEG_START_EVENT,
    SEG_START_DONE_EVENT,
    SEG_STOP_EVENT,
    SEG_STOP_DONE_EVENT,
    SEG_STATUS_EVENT,

    /* segment msg */
    SEGMENT_REQUEST_MSG_EVENT = 100,
    SEGMENT_REPLY_MSG_EVENT,
    HOST_REQUEST_MSG_EVENT,
    ARRAY_REQUEST_MSG_EVENT,
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

    GET_IDENTITY_CMDSTATE,

    HTTP_REQUEST_CMDSTATE,

    SEG_INIT_CMDSTATE,
    SEG_INITDB_CMDSTATE,
    SEG_START_CMDSTATE,
    SEG_STOP_CMDSTATE,
    

    CMDSTATE_MAX,
};

extern const char* cmdstate_str(cmdstate_t cmdstate);


#endif /* ENUM_H */
