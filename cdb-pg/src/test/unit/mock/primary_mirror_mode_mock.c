/*
 * Manually created mocking source
 *
 * This file is not automatically updated by generate_mocks.py
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"
#include "postmaster/postmaster.h"
#include "postmaster/primary_mirror_mode.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "miscadmin.h"
#include "access/xlog.h"
#include "utils/guc.h"
#include "utils/netcheck.h"
#include "catalog/pg_filespace.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbvars.h"
#include <string.h>
#include "access/slru.h"
#include "access/xlog_internal.h"

const char     *
getMirrorModeLabel(PrimaryMirrorMode mode)
{
	check_expected(mode);

	return (char *) (unsigned) mock();
}

const char     *
getDataStateLabel(DataState_e state)
{
	check_expected(state);

	return (char *) (unsigned) mock();
}

const char     *
getSegmentStateLabel(SegmentState_e state)
{
	check_expected(state);

	return (char *) (unsigned) mock();
}

const char     *
getFaultTypeLabel(FaultType_e faultType)
{
	check_expected(faultType);

	return (char *) (unsigned) mock();
}

const char     *
getTransitionResultLabel(PrimaryMirrorModeTransitionResult res)
{
	check_expected(res);

	return (char *) (unsigned) mock();
}

char 
getRole(PrimaryMirrorMode mode)
{
	check_expected(mode);

	return (char) mock();
}

char 
getMode(DataState_e state)
{
	check_expected(state);

	return (char) mock();
}

PrimaryMirrorMode 
decipherPrimaryMirrorModeArgument(const char *arg)
{
	check_expected(arg);

	return (PrimaryMirrorMode) mock();
}

PrimaryMirrorModeTransitionArguments *
createNewTransitionArguments(void)
{


	return (PrimaryMirrorModeTransitionArguments *) (unsigned) mock();
}

void 
getPrimaryMirrorStatusCodes(PrimaryMirrorMode * pm_mode, SegmentState_e * s_state, DataState_e * d_state, FaultType_e * f_type)
{
	check_expected(pm_mode);
	check_expected(s_state);
	check_expected(d_state);
	check_expected(f_type);
	optional_assignment(pm_mode);
	optional_assignment(s_state);
	optional_assignment(d_state);
	optional_assignment(f_type);
	mock();
}

void 
getPrimaryMirrorModeStatus(char *bufOut, int bufSize)
{
	check_expected(bufOut);
	check_expected(bufSize);
	optional_assignment(bufOut);
	mock();
}

void 
primaryMirrorGetFilerepArguments(FileRepRole_e * fileRepRoleOut, SegmentState_e * segmentStateOut, DataState_e * dataStateOut, char **primaryHostAddressOut, int *primaryPortOut, char **mirrorHostAddressOut, int *mirrorPortOut, bool * fullSyncOut)
{
	check_expected(fileRepRoleOut);
	check_expected(segmentStateOut);
	check_expected(dataStateOut);
	check_expected(primaryHostAddressOut);
	check_expected(primaryPortOut);
	check_expected(mirrorHostAddressOut);
	check_expected(mirrorPortOut);
	check_expected(fullSyncOut);
	optional_assignment(fileRepRoleOut);
	optional_assignment(segmentStateOut);
	optional_assignment(dataStateOut);
	optional_assignment(primaryHostAddressOut);
	optional_assignment(primaryPortOut);
	optional_assignment(mirrorHostAddressOut);
	optional_assignment(mirrorPortOut);
	optional_assignment(fullSyncOut);
	mock();
}

bool 
isFullResync(void)
{


	return (bool) mock();
}

void 
setFullResync(bool isFullResync)
{
	check_expected(isFullResync);

	mock();
}

bool 
isResyncRunning(void)
{


	return (bool) mock();
}

void 
setResyncCompleted()
{


	mock();
}

bool 
isDatabaseRunning(void)
{


	return (bool) mock();
}

void 
getFileRepRoleAndState(FileRepRole_e * fileRepRoleOut, SegmentState_e * segmentStateOut, DataState_e * dataStateOut, bool * isInFilerepTransitionOut, DataState_e * transitionTargetDataStateOut)
{
	check_expected(fileRepRoleOut);
	check_expected(segmentStateOut);
	check_expected(dataStateOut);
	check_expected(isInFilerepTransitionOut);
	check_expected(transitionTargetDataStateOut);
	optional_assignment(fileRepRoleOut);
	optional_assignment(segmentStateOut);
	optional_assignment(dataStateOut);
	optional_assignment(isInFilerepTransitionOut);
	optional_assignment(transitionTargetDataStateOut);
	mock();
}

bool 
isPrimaryMirrorModeAFullPostmaster(bool checkTargetModeAsWell)
{
	check_expected(checkTargetModeAsWell);

	return (bool) mock();
}

bool 
isInFaultFromPostmasterReset()
{


	return (bool) mock();
}

bool 
doesPrimaryMirrorModeRequireFilerepProcess(bool checkTargetModeAsWell)
{
	check_expected(checkTargetModeAsWell);

	return (bool) mock();
}

Size 
primaryMirrorModeShmemSize(void)
{


	return (Size) mock();
}

void 
primaryMirrorModeShmemInit(void)
{


	mock();
}

void 
setInitialRequestedPrimaryMirrorMode(PrimaryMirrorMode targetMode)
{
	check_expected(targetMode);

	mock();
}

void 
setFaultAfterReset(bool val)
{
	check_expected(val);

	mock();
}

void 
setTransitionToFault()
{


	mock();
}

bool 
getTransitionToFault()
{


	return (bool) mock();
}

void 
primaryMirrorSetInSync()
{


	mock();
}

bool 
primaryMirrorCheckNICFailure()
{


	return (bool) mock();
}

bool 
primaryMirrorPostmasterResetShouldRestartPeer(void)
{


	return (bool) mock();
}

void 
primaryMirrorHandlePostmasterReset(void)
{


	mock();
}

PrimaryMirrorModeTransitionResult 
requestTransitionToPrimaryMirrorMode(PrimaryMirrorModeTransitionArguments * args, char extraResultInfoOut[256])
{
	check_expected(args);
	check_expected(extraResultInfoOut);
	optional_assignment(args);
	return (PrimaryMirrorModeTransitionResult) mock();
}

int64 
getChangeTrackingSessionId(void)
{


	return (int64) mock();
}

bool 
updateDataState(DataState_e dataState)
{
	check_expected(dataState);

	return (bool) mock();
}

bool 
updateSegmentState(SegmentState_e segmentState, FaultType_e faultType)
{
	check_expected(segmentState);
	check_expected(faultType);

	return (bool) mock();
}

bool 
isPrimaryMirrorModeTransitionRequested(void)
{


	return (bool) mock();
}

void 
copyTransitionParametersToLocalMemory()
{


	mock();
}

void 
doRequestedPrimaryMirrorModeTransitions(bool isInShutdown)
{
	check_expected(isInShutdown);

	mock();
}

void 
primaryMirrorRecordSegmentStateToPostmasterLocalMemory(void)
{


	mock();
}

void 
primaryMirrorSetIOSuspended(bool ioSuspended)
{
	check_expected(ioSuspended);

	mock();
}

bool 
primaryMirrorIsIOSuspended(void)
{


	return (bool) mock();
}

void 
primaryMirrorSetBGWriterPID(pid_t pid)
{
	check_expected(pid);

	mock();
}

pid_t 
primaryMirrorGetBGWriterPID(void)
{


	return (pid_t) mock();
}

Oid 
primaryMirrorGetTempFilespaceOID(void)
{


	return (Oid) mock();
}

Oid 
primaryMirrorGetTxnFilespaceOID(void)
{


	return (Oid) mock();
}

char           *
primaryMirrorGetTxnFilespacePath(void)
{


	return (char *) (unsigned) mock();
}

char           *
primaryMirrorGetTempFilespacePath(void)
{


	return (char *) (unsigned) mock();
}

bool 
primaryMirrorIsUsingDefaultFilespaceForTempFiles(void)
{


	return (bool) mock();
}

bool 
primaryMirrorIsUsingDefaultFilespaceForTxnFiles(void)
{


	return (bool) mock();
}

char           *
makeRelativeToTxnFilespace(char *path)
{
	check_expected(path);
	optional_assignment(path);
	return (char *) (unsigned) mock();
}

char           *
primaryMirrorGetPeerTxnFilespacePath(void)
{


	return (char *) (unsigned) mock();
}

char           *
makeRelativeToPeerTxnFilespace(char *path)
{
	check_expected(path);
	optional_assignment(path);
	return (char *) (unsigned) mock();
}

bool 
isFilespaceInfoConsistent(void)
{


	return (bool) mock();
}

void 
primaryMirrorPopulateFilespaceInfo(void)
{


	mock();
}

bool 
isTxnDir(char *path)
{
	check_expected(path);
	optional_assignment(path);
	return (bool) mock();
}

int 
isAbsolutePath(const char *path)
{
	check_expected(path);

	return (int) mock();
}

bool 
isFilespaceUsedForTempFiles(Oid fsoid)
{
	check_expected(fsoid);

	return (bool) mock();
}

bool 
isFilespaceUsedForTxnFiles(Oid fsoid)
{
	check_expected(fsoid);

	return (bool) mock();
}

void 
primaryMirrorModeResetSpinLocks(void)
{


	mock();
}
