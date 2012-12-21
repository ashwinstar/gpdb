#ifndef SENDALERT_COMMON_H
#define SENDALERT_COMMON_H

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

int set_alert_severity(const GpErrorData * errorData,
	char *subject,
	bool *send_via_email,
	char *email_priority,
	bool *send_via_snmp,
	char *snmp_severity,
	bool *send_via_connectemc,
	EmcConnectSeverity *connectemc_severity,
	EmcConnectDetailedSymptom *connectemc_symptom);


#endif
