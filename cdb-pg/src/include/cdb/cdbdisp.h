/*-------------------------------------------------------------------------
 *
 * cdbdisp.h
 * routines for dispatching commands from the dispatcher process
 * to the qExec processes.
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBDISP_H
#define CDBDISP_H

/*
 * Enumeration for defining the target subset of all segdbs 
 * to send a command to.  This is determined by considering the
 * "scope" of a command.  For example, 
 * 		1) a create user command has listener scope. Use DTS_LISTENER
 * 		2) A create table command has database scope, and should include
 * 			secondaries as well as primaries.  Use DTS_SEGDB.
 * 		3) A select stament would have database scope but only one per segindex.
 * 			Use DTS_SEGMENT.
 */
typedef enum
{
	DTS_SEGDB,		/* Send to all connected segdbs */
	DTS_SEGMENT,	/* Send to only one segdb per segindex */
	DTS_LISTENER	/* Send ot only one segdb per listener */
} DISPATCH_TARGET_SET;

/*
 * CdbDispatchCommand:
 * Send ths szCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the targetSet parameter.  cancelFromBelow indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.  They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable cdb_connections_per_thread.
 */
extern void CdbDispatchCommand( const char *szCommand, DISPATCH_TARGET_SET targetSet, bool cancelFromBelow );

/*
 * CdbCheckDispatchResult:
 * Waits for threads that were launched in CdbDispatchCommand
 * to finish, and then checks to see whether they all succeeded.
 * If so, returns true.  If not, returns false, and fills in pszError 
 * out parameter with a concatenation of errors from any failed command.
 */
extern bool CdbCheckDispatchResult( char **pszError );


#endif	/* CDBDISP_H */
