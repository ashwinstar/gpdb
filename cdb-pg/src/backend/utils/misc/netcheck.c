/*-------------------------------------------------------------------------
 *
 * netcheck.c
 *	  Implementation of network checking utilities
 *
 * Copyright (c) 2011, EMC Corp.
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#if defined(__darwin__) || defined(sun)
#include <sys/sockio.h>
#endif /* defined(__darwin__) || defined(sun) */

#include "postgres.h"
#include "utils/netcheck.h"
#include "utils/simex.h"
#include "utils/timestamp.h"


/*
 * CONSTANTS
 */

#define NET_RETRIES             (5)
#define IF_CONFIG_BUFFER_SIZE   (32 * 1024)


/*
 * STATIC VARIABLES
 */

/* socket used for retrieving NIC state */
static int sock = -1;

/* interface configuration */
static struct ifconf ifconfig;

/* buffer used for storing configuration */
static char ifBuffer[IF_CONFIG_BUFFER_SIZE];

/* host name to be matched to NIC */
static const char *hostTarget = NULL;

/* IP address to be matched to NIC */
static in_addr_t ipTarget;

/* flag indicating if NIC is running */
static bool running = false;


/*
 * FUNCTION PROTOTYPES
 */

static void Init(const char *hostname);
static bool GetHost(void);
static bool OpenSocket(void);
static bool GetIFConfig(void);
static bool FindNIC(void);
static bool CheckRunning(void);
static void Release(void);


/*
 * check if the NIC used for routing to the given host is running
 */
bool
NetCheckNIC(const char *hostname)
{
	/* open a socket */
	for (int i = 0; i < NET_RETRIES; i++)
	{
		Init(hostname);

		if (GetHost() &&
		    OpenSocket() &&
		    GetIFConfig() &&
		    FindNIC())
		{
			Release();
			return CheckRunning();
		}

		elog(LOG, "retry %d to check NIC status.", i + 1);
		Release();

		/* sleep for 1 second to avoid tight loops */
		pg_usleep(USECS_PER_SEC);
	}

	elog(LOG, "Failed to check NIC status after trying %d times.", NET_RETRIES);

	return false;
}


/*
 * Initialize static variables.
 */
static void
Init(const char *hostname)
{
	sock = -1;
	hostTarget = hostname;
	running = false;
	memset(&ipTarget, 0, sizeof(ipTarget));
	memset(ifBuffer, 0, sizeof(ifBuffer));

	memset(&ifconfig, 0, sizeof(ifconfig));
	ifconfig.ifc_buf = ifBuffer;
	ifconfig.ifc_len = sizeof(ifBuffer);
}


/*
 * Retrieve IP address for the target host name.
 */
static bool
GetHost()
{
	struct hostent *hp = gethostbyname(hostTarget);
	if (NULL == hp)
	{
		elog(LOG, "Failed to lookup host name to retrieve NIC configuration.");
		return false;
	}

	memcpy(&ipTarget, hp->h_addr, sizeof(ipTarget));
	return true;
}


/*
 * Open socket.
 */
static bool
OpenSocket()
{
	sock = gp_socket(AF_INET, SOCK_DGRAM, 0 /* protocol */);

	if (-1 == sock)
	{
		elog(LOG, "Failed to open socket to retrieve NIC configuration.");
		return false;
	}

	return true;
}


/*
 * Retrieve interface configuration.
 */
static bool
GetIFConfig()
{
	if (-1 == ioctl(sock, SIOCGIFCONF, &ifconfig))
	{
		elog(LOG, "Failed to retrieve NIC configuration.");
		return false;
	}
	return true;
}


/*
 * Find the NIC that is used for routing to the target IP.
 */
static bool
FindNIC()
{
	/*
	 * ifreq structures are variable length;
	 * we have to step along the structure by the size of
	 * the previous structure
	 */
	struct ifreq *ifr = (struct ifreq *) ifconfig.ifc_req;
	struct ifreq *end = (struct ifreq *) ((char *) ifr + ifconfig.ifc_len);

	while (ifr < end)
	{
		struct ifreq *ifrCurrent = ifr;

#ifdef __darwin__
		/* step along the array by the size of the current structure */
		ifr = (struct ifreq *)((char *)ifr + ifr->ifr_addr.sa_len + IFNAMSIZ);
#else
		ifr++;
#endif /* __darwin__ */

		/* get the flags for this interface */
		if (-1 == ioctl(sock, SIOCGIFFLAGS, ifrCurrent))
		{
			elog(LOG, "Failed to retrieve configuration flags for NIC %s.", ifrCurrent->ifr_name);
			return false;
		}

		/* check if the examined interface is a NIC */
		if ((ifrCurrent->ifr_flags &
			  (IFF_UP | IFF_BROADCAST | IFF_POINTOPOINT | IFF_LOOPBACK | IFF_NOARP))
			!= (IFF_UP | IFF_BROADCAST))
		{
			continue;
		}

		in_addr_t nicAddress = ((struct sockaddr_in *) &ifrCurrent->ifr_addr)->sin_addr.s_addr;
		int nicRunning = ifrCurrent->ifr_flags & IFF_RUNNING;

		/* get NIC mask */
		if (-1 == ioctl(sock, SIOCGIFNETMASK, ifrCurrent))
		{
			elog(LOG, "Failed to retrieve network mask for NIC %s.", ifrCurrent->ifr_name);
			return false;
		}
		in_addr_t mask = ((struct sockaddr_in *) &ifrCurrent->ifr_addr)->sin_addr.s_addr;

		/* check if IP addresses match under NIC mask */
		if ((ipTarget & mask) == (nicAddress & mask))
		{
			running = nicRunning;
			if (nicRunning == false)
			{
				elog(LOG, "Detected failure on NIC %s.", ifrCurrent->ifr_name);
			}

			return true;
		}
	}

	Assert(!running);
	elog(LOG, "No NIC matches host name %s.", hostTarget);

	return true;
}


/*
 * Check if NIC is running.
 */
static bool
CheckRunning()
{
	return running;
}


/*
 * Release allocated resources.
 */
static void
Release()
{
	(void) close(sock);
}


/* EOF */
