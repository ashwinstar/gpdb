#ifndef _SENDALERT_H
#define _SENDALERT_H

#include "postmaster/syslogger.h"

extern int send_alert(PipeProtoChunk *chunk);
#endif 
