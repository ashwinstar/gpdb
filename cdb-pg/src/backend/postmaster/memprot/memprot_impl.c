/* 
 * memprot_impl.c
 * 		Memory protection server implementation.
 * 
 * Copyright (c) 2008, Greenplum Inc.
 *
 * memproted server implementation.  The service routine that serves
 * mem request, and some fault injection handling.
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

#include "utils/faultinjection.h"
#include "memprot_impl.h"

static MemProtServer_ImplConfig g_mprot_config = {0, 0};

void memprot_init_impl_config()
{
	Assert(gp_vmem_protect_limit >= 0);
	g_mprot_config.vmem_max = ((int64) gp_vmem_protect_limit) << 20;
	Assert(gp_vmem_protect_limit >= 0);

	g_mprot_config.vmem_alloc = 0;
}

static int64 mprot_faultinj_config(int32 reason, int64 sz); 

int memprot_service_request(MemProtectRequest *req, MemProtectResponse *res)
{
	Assert(req && res);

	switch(req->command)
	{
		case MemProt_Request:
			if(g_mprot_config.vmem_max == 0 ||
				g_mprot_config.vmem_alloc + req->req_size < g_mprot_config.vmem_max)
			{
				res->command = MemProt_Grant;
				res->grant_size = req->req_size;

				g_mprot_config.vmem_alloc += req->req_size;
			}
			else
			{
				res->command = MemProt_Deny;
				res->grant_size = 0;
			}

			return 0;

		case MemProt_Release:
			{
				Assert(g_mprot_config.vmem_alloc >= req->req_size);
				g_mprot_config.vmem_alloc -= req->req_size;

				res->command = MemProt_Grant;
				res->grant_size = req->req_size;
				return 0;
			}

		case MemProt_ConfigFault: 
			{
				int64 confval = mprot_faultinj_config(req->reason, req->req_size); 
				if(confval >= 0)
					res->command = MemProt_Grant;
				else
					res->command = MemProt_Deny;
				res->grant_size = confval;
				return 0;
			}

		/* Unknow command */ 
		default: 
			return -1; 
	} 
	/* Success */
	return 0;
}

/* Hack in fault injection config */
static int64 mprot_faultinj_config(int32 reason, int64 size)
{
	switch(reason)
	{
		/* config mp */
		case GP_FAULT_USER_CONFIG_MP:
		{
			/* Current don't have any neg config code */
			if(size < 0)
				return -1;

			switch(size) 
			{
				case 0:
					/* set vmem_max to 0, ie. disable mem protect */
					g_mprot_config.vmem_max = 0;
					return 0;
				case 1:
					/* Query vmem_max */
					return g_mprot_config.vmem_max;
				case 2:
					/* Query vmem_alloc */
					return g_mprot_config.vmem_alloc;

					/* Case 3 to 20 is not used */
				default:
					{
						if(size >= 20 && size < 63)
						{
							/* set vmem_max to 1 << configcode. 
							 * NOTE: This may make vmem_alloc > vmem_max.  If this happens, next memrequest
							 * will fail.
							 */
							g_mprot_config.vmem_max = 1 << size; 
							return g_mprot_config.vmem_max;
						}

						/* code 64 and above are not used */
						return -1;
					}
			}
		}

		case GP_FAULT_USER_MP_SEGV:
			*(int *) 0 = 1234;
			break;
		case GP_FAULT_USER_MP_ERROR:
			elog(ERROR, "User fault MP to raise error");
		case GP_FAULT_USER_MP_FATAL:
			elog(FATAL, "User fault MP to raise fatal error");
		case GP_FAULT_USER_MP_PANIC:
			elog(PANIC, "User fault MP to raise panic error");

		default:
			/* Unknown reason code */
			return -1;
	}

	Assert(!"Should never reach");
	return -1;
}
