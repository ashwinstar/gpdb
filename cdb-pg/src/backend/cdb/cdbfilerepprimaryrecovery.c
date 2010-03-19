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
#include "cdb/cdbresynchronizechangetracking.h"
#include "utils/flatfiles.h"

static void FileRepPrimary_StartRecoveryInSync(void);
static void FileRepPrimary_StartRecoveryInChangeTracking(void);

static int FileRepPrimary_RunRecoveryInSync(void);

/****************************************************************
 * FILEREP SUB-PROCESS (FileRep Primary RECOVERY Process)
 ****************************************************************/

void 
FileRepPrimary_StartRecovery(void)
{		
	elog(LOG, "FileRepPrimary_StartRecovery()");
	
	switch (dataState)
	{
		case DataStateInSync:
			FileRepPrimary_StartRecoveryInSync();
			break;
			
		case DataStateInChangeTracking:
			FileRepPrimary_StartRecoveryInChangeTracking();
			break;
			
		case DataStateInResync:
		default:
			Assert(0);
			break;
	}
}	

/****************************************************************
 * DataStateInSync
 ****************************************************************/

/*
 * 
 * FileRepPrimary_StartRecoveryInSync()
 *
 * That process will be started only during recovery if
 *			*) dataState == DataStateInSync or
 *			*) dataState == case DataStateResynchronizing
 *
 */
static void 
FileRepPrimary_StartRecoveryInSync(void)
{	
	int	status = STATUS_OK;
	
	elog(LOG, "FileRepPrimary_StartRecoveryInSync()");
	
	while (1) {
		
		if (status != STATUS_OK) {
			FileRepSubProcess_SetState(FileRepStateFault);
		}
		
		while (FileRepSubProcess_GetState() == FileRepStateFault ||
			   
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

		Insist(dataState == DataStateInSync);
		
		status = FileRepPrimary_RunRecoveryInSync();

		
	} // while(1)	

}

/*
 * 
 * FileRepPrimary_RunRecoveryInSync()
 *		
 *		1) Recover Flat Files
 *			a) pg_control file
 *			b) pg_database file
 *			c) pg_auth file
 *			d) pg_twophase directory
 *			e) Slru directories 
 *					*) pg_clog 
 *					*) pg_multixact 
 *					*) pg_distributedlog
 *					*) pg_distributedxidmap
 *					*) pg_subtrans
 *
 *		2) Reconcile xlog EOF
 *
 */
static int 
FileRepPrimary_RunRecoveryInSync(void)
{	
	int status = STATUS_OK;

	elog(LOG, "FileRepPrimary_RunRecoveryInSync()");
	
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
		elog(DEBUG1, "FileRepPrimary_RunRecovery() SlruRecoverMirror");
		
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

/****************************************************************
 * DataStateInChangeTracking
 ****************************************************************/

/*
 * 
 * FileRepPrimary_StartRecoveryInChangeTracking()
 *
 */
static void 
FileRepPrimary_StartRecoveryInChangeTracking(void)
{		
	elog(LOG, "FileRepPrimary_StartRecoveryInChangeTracking()");
	
	while (1) {
					
		while (FileRepSubProcess_GetState() == FileRepStateFault) {			
			FileRepSubProcess_ProcessSignals();
			pg_usleep(50000L); /* 50 ms */	
		}
		
		if (FileRepSubProcess_GetState() == FileRepStateShutdown ||
			FileRepSubProcess_GetState() == FileRepStateImmediateShutdown) {
			
			break;
		}
				
		Insist(fileRepRole == FileRepPrimaryRole);
		Insist(dataState == DataStateInChangeTracking);		
		Insist(FileRepSubProcess_GetState() != FileRepStateReady);
		
		ChangeTracking_CreateInitialFromPreviousCheckpoint();
		
		/* NOTE: Any error during change tracking will result in disabling Change Tracking */
		FileRepSubProcess_SetState(FileRepStateReady);
		
		break;
		
	} // while(1)	
}
