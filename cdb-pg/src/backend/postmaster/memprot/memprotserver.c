/* 
 * memprotserver.c
 * 		Memory protection server.
 * 
 * Copyright (c) 2008, Greenplum Inc.
 *
 * The memprot server is started by postmaster.  It listens to
 * the request from each postgres process, and responses whether
 * a memory request is granted (how much), or denied.
 *
 * The impl uses Matt's service framework.  
 * See postmaster/service.h.
 */

#include "postgres.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "utils/elog.h"

#include "miscadmin.h"
#include "libpq/pqsignal.h"
#include "cdb/cdbvars.h"
#include "commands/sequence.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "catalog/pg_control.h"
#include "postmaster/service.h"
#include "postmaster/memprotserver.h"
#include "storage/ipc.h"

#include "memprot_impl.h"

/* 
 * Function prototypes 
 */
#ifdef EXEC_BACKEND
static pid_t memprotserver_forkexec(void);
#endif

static void MemProtect_ServiceClientTimeout(struct timeval *timeout);
static void MemProtect_ServiceRequestShutdown(SIGNAL_ARGS);
static void MemProtect_ServiceInit(int listener_port);
static bool MemProtect_ServiceShutdownRequested(void);
static bool MemProtect_ServiceRequest(ServiceCtrl *service_ctrl, int sockfd, uint8 *request);
static void MemProtect_ServiceShutdown(void);

/* 
 * Global state var
 */
static bool am_memprot_server = false;
static ServiceConfig MemProtectServer_ServiceConfig = 
{
	"MP",
	"Memory Protection Server", "Memory Protection Server Process", "memprot",
	sizeof(MemProtectRequest), sizeof(MemProtectResponse),
	MemProtect_ServiceClientTimeout,
	MemProtect_ServiceRequestShutdown,
	NULL, /* Early init */
	NULL, /* PostgresInit */
	MemProtect_ServiceInit,
	MemProtect_ServiceShutdownRequested,
	MemProtect_ServiceRequest,
	MemProtect_ServiceShutdown,
	NULL, /* PostmasterDied callback */
};
static ServiceCtrl MemProtect_ServiceCtrl;

typedef struct MemProtectServerShmem
{
	int listener_port;
} MemProtectServerShmem;

static MemProtectServerShmem *memprot_servershared = NULL;

int MemProtectServerShmemSize()
{
	return sizeof(MemProtectServerShmem);
}

void MemProtectServerShmemInit()
{
	bool found;

	memprot_servershared = (MemProtectServerShmem *) ShmemInitStruct(
			"Memory Protection Server", 
			MemProtectServerShmemSize(),
			&found);

	if(!found)
		memprot_servershared->listener_port = -1;
}

extern bool MemProtect_Do(MemProtectRequest *request, MemProtectResponse *response)
{
	bool connected;
	bool success;

	connected = MemProtectServerClientConnect();

	if(!connected)
		return false;

	success = MemProtectServerClientSendRequest(request);

	if(success)
	{
		struct timeval timeout;
		ServiceGetClientTimeout(&MemProtectServer_ServiceConfig, &timeout);
		success = MemProtectServerClientReceiveResponse(response, &timeout);
	}

	return success;
}

int memprotect_start()
{
	pid_t mp_server_pid;

#ifdef EXEC_BACKEND
	switch((mp_server_pid = memprotserver_forkexec()))
#else
	switch((mp_server_pid = fork_process()))
#endif
	{
		case -1:
			ereport(LOG, (errmsg("Could not force memory protect server: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			ClosePostmasterPorts(false);

			ServiceInit(&MemProtectServer_ServiceConfig, &MemProtect_ServiceCtrl);
			ServiceMain(&MemProtect_ServiceCtrl);

			break;
#endif
		default:
			return (int) mp_server_pid;
	}

	Assert(!"Should never reach");
	return 0;
}

/*
 * Helpers 
 */
#ifdef EXEC_BACKEND
static pid_t memprotectserver_forkexec()
{
	char *av[10];
	int ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkmemprot";
	av[ac++] = NULL; /* filled in by postmaster_forkexec */
	av[ac] = NULL;

	return postmaster_forkexec(ac, av);
}
#endif

static bool memprot_shutdown_requested = false;
static void MemProtect_ServiceRequestShutdown(SIGNAL_ARGS)
{
	memprot_shutdown_requested = true;
}

static void MemProtect_ServiceInit(int listener_port)
{
	if(memprot_servershared == NULL)
		elog(FATAL, "Memory protect server shared memory not initialized");
	
	memprot_servershared->listener_port = listener_port;
	am_memprot_server = true;

	memprot_init_impl_config();

	elog(LOG, "Started");
}

static bool MemProtect_ServiceShutdownRequested(void)
{
	return memprot_shutdown_requested;
}

static void MemProtect_ServiceClientTimeout(struct timeval *timeout)
{
	/* Do we want to GUC it? */
	timeout->tv_sec = 10;
	timeout->tv_usec = 0;
}

static void MemProtect_ServiceShutdown()
{}

static ServiceClient MemProtectClient = {NULL, -1};

bool MemProtectServerClientConnect(void)
{
	bool connected;

	if(memprot_servershared == NULL)
		elog(FATAL, "MemProt server shared memory not initialized");

	connected = ServiceClientConnect(
					&MemProtectServer_ServiceConfig,
					memprot_servershared->listener_port,
					&MemProtectClient,
					true
					);
	return connected;
}

bool MemProtectServerClientSendRequest(MemProtectRequest *request)
{
	return ServiceClientSendRequest(&MemProtectClient, request, sizeof(MemProtectRequest));
}

bool MemProtectServerClientReceiveResponse(MemProtectResponse *response, struct timeval *timeout)
{
	return ServiceClientReceiveResponse(
				&MemProtectClient,
				response, sizeof(MemProtectResponse),
				timeout
				);
}

char* MemProtectRequestCommandToString(MemProtectRequestCommand command)
{
	switch(command)
	{
		case MemProt_Request:
			return "RequestMemory";
		case MemProt_Release:
			return "ReleaseMemory";
		case MemProt_ConfigFault:
			return "ConfigOrFaultMemProtServer";
		default:
			return "Unknown";
	}
}

char *MemProtectResponseCommandToString(MemProtectResponseCommand command)
{
	switch(command)
	{
		case MemProt_Grant:
			return "GrantMemory";
		case MemProt_Deny:
			return "DenyMemoryRequest";
		default:
			return "Unknown";
	}
}

static bool MemProtect_ServiceRequest(ServiceCtrl *serviceCtrl, int sockfd, uint8 *request)
{
	MemProtectRequest *mem_req = (MemProtectRequest *) request;
	MemProtectResponse mem_response;
	bool result = true;

	/* 
	 * Matt: Use try/catch to catch unexpected errors that bubble up
	 * to this level and disable QD mirroring
	 */
	int err = memprot_service_request(mem_req, &mem_response);

	if(err!=0)
		elog(ERROR, "MemProt failed to service request");

	result = ServiceProcessRespond(serviceCtrl, sockfd, (uint8 *)&mem_response, sizeof(mem_response));

	return result;
}
	
