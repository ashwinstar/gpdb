/*
 *  cdbfilerepprimaryrecovery.c
 *  
 *
 *  Copyright 2009-2010 Greenplum Inc. All rights reserved. *
 *
 */


#include "postgres.h"

#include <signal.h>

#include "access/xlog.h"
#include "access/twophase.h"
#include "access/slru.h"
#include "cdb/cdbfilerepprimaryrecovery.h"
#include "cdb/cdbfilerepservice.h"
#include "cdb/cdbfilerepprimaryack.h"
#include "cdb/cdbfilerep.h"
#include "utils/flatfiles.h"

static int FileRepPrimary_RunRecovery(void);

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Primary RECOVERY Process)
 ****************************************************************/
/*
 * 
 * FileRepPrimary_StartRecovery()
 *
 * That process will be started only during recovery if
 *			*) dataState == DataStateInSync or
 *			*) dataState == case DataStateResynchronizing
 *
 */
void 
FileRepPrimary_StartRecovery(void)
{	
	int	status = STATUS_OK;
	
	elog(LOG, "FileRepPrimary_StartRecovery()");
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateSuspend ||
			   FileRepSubProcess_GetState() == FileRepStateFault ||
			   
			   (fileRepShmemArray[0]->state == FileRepStateNotInitialized &&
			    FileRepSubProcess_GetState() != FileRepStateShutdown &&
			    FileRepSubProcess_GetState() != FileRepStateImmediateShutdown)) {
			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
			
			break;
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateReady) {
			break;
		}
		
		Insist(fileRepRole == FileRepPrimaryRole);

		Insist(dataState == DataStateInSync ||
			   dataState == DataStateResynchronizing);
		
		status = FileRepPrimary_RunRecovery();

		
	} // while(1)	

	if (FileRepSubProcess_GetState() == FileRepStateShutdown) {
		/* perform graceful shutdown */
	}
	
}

/*
 * 
 * FileRepPrimary_RunRecovery()
 *		Negotiation Protocol
 *		Recover Flat Files
 *			a) pg_control file
 *			b) pg_database file
 *			c) pg_auth file
 *			d) pg_twophase directory
 *		Reconcile xlog EOF
 *
 * Relation cache files are not copied to mirror. See comment 
 * in front of the routine "RelationCacheInitFileRemove()"
 *
 */

static int 
FileRepPrimary_RunRecovery(void)
{	
	int status = STATUS_OK;

	elog(LOG, "FileRepPrimary_RunRecovery()");
	
	while (1) {
		
		status = XLogRecoverMirrorControlFile();
		elog(DEBUG1, "FileRepPrimary_RunRecovery() XLogRecoverMirrorControlFile");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}		
		
		status = XLogReconcileEofPrimary();
		elog(DEBUG1, "FileRepPrimary_RunRecovery() XLogReconcileEofPrimary");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}

		status = FlatFilesRecoverMirror();
		elog(DEBUG1, "FileRepPrimary_RunRecovery() FlatFilesRecoverMirror");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}		

		status = TwoPhaseRecoverMirror();
		elog(DEBUG1, "FileRepPrimary_RunRecovery() TwoPhaseRecoverMirror");

		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}	
		
		status = SlruRecoverMirror();
		elog(DEBUG1, "FileRepPrimary_RunRecovery() TwoPhaseRecoverMirror");
		
		if (status != STATUS_OK) {
			break;
		}
		
		FileRepSubProcess_ProcessSignals();
		if (FileRepSubProcess_GetState() != FileRepStateInitialization) {
			break;
		}			
		
		
		FileRepSubProcess_SetState(FileRepStateReady);
		break;
	}
	
	
	return status;
}

/*
 * FileRepPrimary_MirrorNegotiation
 *
 * Handshake between primary and mirror to ensure 
 *		*) same protocol version
 *		*) synchronized segment state
 * Handshake is performed via Control Messages.	
 */
/*
static int
FileRepPrimary_MirrorNegotiation(void)
{
*/
	/* Verify State, Protocol Version
	 
	 
	 if (! FileRepConnClient_SendMessage(
	 TRUE, // Control Message Type
	 TRUE, // Synchronous Message
	 fileRepMessage,
	 messageLength)) {
	 
	 ereport(DEBUG1, 
	 (errmsg("Primary Message sent fail.")));
	 
	 return STATUS_ERROR;
	 }
	 
	 */	
/*
	return STATUS_OK;
}
*/
