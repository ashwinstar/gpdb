/*
 *  faultinjector.h
 *  
 *
 *  Copyright 2009-2010, Greenplum Inc. All rights reserved.
 *
 */

#ifndef FAULTINJECTOR_H
#define FAULTINJECTOR_H

#include "pg_config_manual.h"

#define FAULTINJECTOR_MAX_SLOTS	16

/*
 *
 */
typedef enum FaultInjectorIdentifier_e {
	FaultInjectorIdNotSpecified=0,
	
	FaultInjectorIdAll,
		/* 
		 * affects all faults injected
		 *		*) remove all faults injected from the segment
		 *		*) display status for all faults injected 
		 */
	
	FileRepConsumer,
	
	FileRepSender,
	
	FileRepReceiver,
	
	FileRepFlush,
	
	FileRepResync,
	
	FileRepTransitionToInResync,
	
	FileRepTransitionToInSync,
	
	Checkpoint,
	
	TransactionCommitPass1FromCreatePendingToCreated,
	
	TransactionCommitPass1FromDropInMemoryToDropPending,
	
	TransactionCommitPass1FromAbortingCreateNeededToAbortingCreate,
	
	TransactionAbortPass1FromCreatePendingToAbortingCreate,
	
	TransactionAbortPass1FromAbortingCreateNeededToAbortingCreate,
	
	TransactionCommitPass2FromDropInMemoryToDropPending,
	
	TransactionCommitPass2FromAbortingCreateNeededToAbortingCreate,
	
	TransactionAbortPass2FromCreatePendingToAbortingCreate,
	
	TransactionAbortPass2FromAbortingCreateNeededToAbortingCreate,
	
	FinishPreparedTransactionCommitPass1FromCreatePendingToCreated,
	
	FinishPreparedTransactionCommitPass1FromDropInMemoryToDropPending,
	
	FinishPreparedTransactionCommitPass1FromAbortingCreateNeededToAbortingCreate,
	
	FinishPreparedTransactionAbortPass1FromCreatePendingToAbortingCreate,
	
	FinishPreparedTransactionAbortPass1FromAbortingCreateNeededToAbortingCreate,
	
	FinishPreparedTransactionCommitPass2FromDropInMemoryToDropPending,
	
	FinishPreparedTransactionCommitPass2FromAbortingCreateNeededToAbortingCreate,
	
	FinishPreparedTransactionAbortPass2FromCreatePendingToAbortingCreate,
	
	FinishPreparedTransactionAbortPass2FromAbortingCreateNeededToAbortingCreate,	
	
	/* INSERT has to be done before that line */
	FaultInjectorIdMax,
	
} FaultInjectorIdentifier_e;

/*
 *
 */
typedef enum FaultInjectorType_e {
	FaultInjectorTypeNotSpecified=0,
	
	FaultInjectorTypeSleep,
	
	FaultInjectorTypeFault,
	
	FaultInjectorTypeFatal,
	
	FaultInjectorTypePanic,
	
	FaultInjectorTypeError,
	
	FaultInjectorTypeInfiniteLoop,
	
	FaultInjectorTypeDataCorruption,
	
	FaultInjectorTypeSuspend,
	
	FaultInjectorTypeResume,
	
	FaultInjectorTypeSkip,
	
	FaultInjectorTypeReset,
	
	FaultInjectorTypeStatus,
	
	/* INSERT has to be done before that line */
	FaultInjectorTypeMax,
	
} FaultInjectorType_e;

/*
 *
 */
typedef enum DDLStatement_e {
	DDLNotSpecified=0,
	
	CreateDatabase,
	DropDatabase,
	
	CreateTable,
	AlterTable,
	DropTable,
	
	CreateIndex,
	AlterIndex,
	ReIndex,
	DropIndex,
	
	CreateFilespaces,
	DropFilespaces,
	
	CreateTablespaces,
	DropTablespaces,
	
	Truncate,
	
	Vacuum,
	
	/* INSERT has to be done before that line */
	DDLMax,
} DDLStatement_e;

/*
 *
 */
typedef enum FaultInjectorState_e {
	FaultInjectorStateNotInitialized=0,
		
	FaultInjectorStateWaiting,
		/* Request is waiting to be injected */
	
	FaultInjectorStateTriggered,
		/* Fault is injected */
	
	FaultInjectorStateCompleted,
		/* Fault was injected and completed successfully */
	
	FaultInjectorStateFailed,
		/* Fault was NOT injected */
	
} FaultInjectorState_e;


/*
 *
 */
typedef struct FaultInjectorEntry_s {
	
	FaultInjectorIdentifier_e	faultInjectorIdentifier;
	
	FaultInjectorType_e		faultInjectorType;
	
	int						sleepTime;
		/* in seconds, in use if fault injection type is sleep */
		
	DDLStatement_e			ddlStatement;
	
	char					databaseName[NAMEDATALEN];
	
	char					tableName[NAMEDATALEN];
	
	volatile	int			occurrence;
	
	volatile	FaultInjectorState_e	faultInjectorState;
		/* the state of the fault injection */
	char					bufOutput[2500];
	
} FaultInjectorEntry_s;


extern FaultInjectorType_e	FaultInjectorTypeStringToEnum(
									char*		faultTypeString);

extern	FaultInjectorIdentifier_e FaultInjectorIdentifierStringToEnum(
									char*			faultName);

extern DDLStatement_e FaultInjectorDDLStringToEnum(
									char*	ddlString);

extern Size FaultInjector_ShmemSize(void);

extern void FaultInjector_ShmemInit(void);

extern bool FaultInjector_InjectFaultIfSet(
							   FaultInjectorIdentifier_e identifier,
							   DDLStatement_e			 ddlStatement,
							   char*					 databaseName,
							   char*					 tableName);

extern int FaultInjector_SetFaultInjection(
							FaultInjectorEntry_s	*entry);


extern bool FaultInjector_IsFaultInjected(
							FaultInjectorIdentifier_e identifier);

#endif	/* FAULTINJECTOR_H */

