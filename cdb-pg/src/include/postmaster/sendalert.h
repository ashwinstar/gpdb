#ifdef SENDALERT_H
#define SENDALERT_H

#include "postmaster/syslogger.h"

extern int send_alert(PipeProtoChunk *chunk);
#endif 
