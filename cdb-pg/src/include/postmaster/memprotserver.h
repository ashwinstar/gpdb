/*
 * Memory Protection Server
 *
 * Copyright (c) 2008, Greenplum Inc.
 */

#ifndef MEM_PROTECT_SERVER_H
#define MEM_PROTECT_SERVER_H

typedef enum MemProtectRequestCommand
{
	MemProt_Request = 0,				/* Request memory */
	MemProt_Release,					/* Release memory */
	MemProt_ConfigFault, 				/* Configue or fault the mp server */ 
	MaxMemProtectRequestCommand 		/* Always last one. */
} MemProtectRequestCommand;

typedef struct MemProectRequest
{
	MemProtectRequestCommand command; 	/* Request command */
	pid_t pid;							/* Who am I */
	int32 reason;						/* Reason code, for config/fault etc */
	int64 req_size;						/* Mem req size */
} MemProtectRequest;

typedef enum MemProtectResponseCommand
{
	MemProt_Grant = 0,					/* Alloc granted */
	MemProt_Deny,						/* Alloc denied */
	MaxMemProtectResponseCommand 		/* Always last one */
} MemProtectResponseCommand;

typedef struct MemProtectResponse
{
	MemProtectResponseCommand command; 	/* Response command */
	int64 grant_size;					/* size granted 	*/
} MemProtectResponse;

extern int memprotect_start(void);
extern bool IsMemProtectProcess(void);
extern int MemProtectServerShmemSize(void);
extern void MemProtectServerShmemInit(void);
extern bool MemProtectServerClientConnect(void);
extern bool MemProtectServerClientSendRequest(MemProtectRequest *memprotect_request);
extern bool MemProtectServerClientReceiveResponse(MemProtectResponse *memprot_response, struct timeval *timeout);
extern char* MemProtectRequestCommandToString(MemProtectRequestCommand command);
extern char* MemProtectResponseCommandToString(MemProtectResponseCommand command);

extern bool MemProtect_Do(MemProtectRequest *request, MemProtectResponse *response);
#endif
