#include <gpcc.h>
#include "enum.h"

#define CASESTR(x)  case x: return APR_STRINGIFY(x)

const char* msg_str(msg_t msg)
{
    switch (msg) {
	CASESTR(NULL_MSG);
	CASESTR(GET_PROTO_MSG);
	CASESTR(PROTO_MSG);
    case MSG_MAX: break;
    }
    return "UNKNOWN_MSG";
}

const char* role_str(role_t role)
{
    switch (role) {
	CASESTR(NULL_ROLE);
	CASESTR(MA_ROLE);
	CASESTR(HA_ROLE);
	CASESTR(QD_ROLE);
	CASESTR(QE_ROLE);
    case ROLE_MAX: break;
    }
    return "UNKNOWN_ROLE";
}


const char* event_str(event_t event)
{
    switch (event) {
	CASESTR(NULL_EVENT);
	CASESTR(TICK_EVENT);
	CASESTR(TIMEUP_EVENT);
	CASESTR(SUCCESS_EVENT);
	CASESTR(FAILURE_EVENT);
	CASESTR(BREAKOUT_EVENT);
	CASESTR(CONN_OPENED_EVENT);
	CASESTR(CONN_CLOSED_EVENT);

	CASESTR(GET_PROTO_MSG_EVENT);
	CASESTR(PROTO_MSG_EVENT);
    case EVENT_MAX: break;
    }
    return "UNKNOWN_EVENT";
}


const char* cmdstate_str(cmdstate_t cmdstate)
{
    switch (cmdstate) {
	CASESTR(NULL_CMDSTATE);
	CASESTR(SYSTEM_CMDSTATE);
	CASESTR(GET_PROTO_CMDSTATE);
    case CMDSTATE_MAX: break;
    }
    return "UNKNOWN_CMDSTATE";
}
