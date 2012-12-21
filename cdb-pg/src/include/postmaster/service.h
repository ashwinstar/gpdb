/*-------------------------------------------------------------------------
 *
 * server.c
 *	  Shared code module that supports Greenplum system server processes
 *    under the postmaster that communicate with other Postgres processes
 *    with TCP/IP (same GP instance or different GP instance).
 *
 * Copyright (c) 2007-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef SERVICE_H
#define SERVICE_H

#ifndef _WIN32
#include <stdint.h>             /* uint32_t (C99) */
#else
typedef unsigned int uint32_t;
#endif 

typedef struct ServiceCtrl
{

	void    *serviceConfig;

	int 	listenerFd;

	int		listenerPort;
	
}	ServiceCtrl;


#define SERVER_EYE_CATCHER_LEN 6

typedef struct ServiceConfig
{
	char    eyeCatcher[SERVER_EYE_CATCHER_LEN];

	char		*title;
	char		*psTitle;
	char		*forkTitle;

	int			requestLen;
	int			responseLen;

	void		(*ServiceClientTimeout) (struct timeval *clientTimeout);
	void		(*ServiceRequestShutdown) (SIGNAL_ARGS);
	void		(*ServiceEarlyInit) (void);
	void		(*ServicePostgresInit) (void);
	void		(*ServiceInit) (int listenerPort);
	bool		(*ServiceShutdownRequested) (void);
	bool		(*ServiceRequest) (ServiceCtrl *serviceCtrl, int sockfd, uint8 *request);
	void		(*ServiceShutdown) (void);
	void		(*ServicePostmasterDied) (void);
	
}	ServiceConfig;

typedef struct ServiceClient
{
	ServiceConfig    *serviceConfig;

	int 	sockfd;
	
}	ServiceClient;

extern void ServiceInit(ServiceConfig *serviceConfig, ServiceCtrl *serviceCtrl);
extern void ServiceMain(ServiceCtrl *serviceCtrl);
extern void ServiceGetClientTimeout(ServiceConfig *serviceConfig, struct timeval *timeout); 
extern bool ServiceClientConnect(ServiceConfig *serviceConfig, int listenerPort, ServiceClient *serverClient, bool complain);
extern bool ServiceClientSendRequest(ServiceClient *serverClient, void* request, int requestLen);
extern bool ServiceClientReceiveResponse(ServiceClient *serverClient, void* response, int responseLen, struct timeval *timeout);
extern bool ServiceClientPollResponse(ServiceClient *serverClient, void* response, int responseLen, bool *pollResponseReceived);
extern char *ServiceGetLastClientErrorString(void);
extern bool ServiceProcessRespond(ServiceCtrl *serviceCtrl, int sockfd, uint8 *response, int responseLen);

#ifdef EXEC_BACKEND
extern pid_t ServiceForkExec(ServiceConfig *serviceConfig);
#endif

#endif   /* SERVICE_H */
