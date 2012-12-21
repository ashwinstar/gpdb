#ifndef _SENDALERT_H
#define _SENDALERT_H

#include "postmaster/syslogger.h"

#ifdef USE_CONNECTEMC
#include "emcconnect/api.h"
#else
typedef enum EmcConnectSeverity
{
    EMC_CONNECT_SEVERITY_UNKNOWN = 0,
} EmcConnectSeverity;

typedef enum EmcConnectDetailedSymptom
{
    EMC_CONNECT_DETAILED_SYMPTOM_NULL = 0,

} EmcConnectDetailedSymptom;
#endif

#define MAX_ALERT_STRING 127

extern int send_alert(const GpErrorData * errorData);

extern int send_alert_from_chunks(const PipeProtoChunk *chunk, const PipeProtoChunk * saved_chunks_in);
extern int set_alert_severity(const GpErrorData * errorData,
						char *subject,
						bool *send_via_email,
						char *email_priority,
						bool *send_via_snmp,
						char *snmp_severity,
						bool *send_via_connectemc,
						EmcConnectSeverity *connectemc_severity,
						EmcConnectDetailedSymptom *connectemc_symptom);
#endif 
