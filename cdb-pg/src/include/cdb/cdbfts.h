/*-------------------------------------------------------------------------
 * cdbfts.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBFTS_H
#define CDBFTS_H

#include "storage/lwlock.h"
#include "cdb/cdbconn.h"
#include "utils/guc.h"
#include "cdb/cdbgang.h"

typedef enum QDMIRRORState
{
	QDMIRROR_STATE_NONE = 0,
	QDMIRROR_STATE_NOTCONFIGURED,
	QDMIRROR_STATE_POSITIONINGTOEND,
	QDMIRROR_STATE_CATCHUPPENDING,
	QDMIRROR_STATE_CATCHINGUP,
	QDMIRROR_STATE_ENABLED,
	QDMIRROR_STATE_DISABLED,
	QDMIRROR_STATE_UNEXPECTEDERROR,
	QDMIRROR_STATE_MAX /* must always be last */
} QDMIRRORState;

struct FtsQDMirrorInfo
{
	bool			configurationChecked;
	QDMIRRORState 	state;

	bool			haveNewCheckpointLocation;
	XLogRecPtr		newCheckpointLocation;
	
	/* stored copies of QD parameters, required so that WalSendServer
	 * process knows how to connect */
	char			name[256];
	uint16			port;
};

struct FtsSegDBState
{
	bool	valid;
	bool	primary;
	int16	id;
};

#define FTS_MAX_TRANSIENT_STATE 100

typedef struct FtsControlBlock
{
	bool		ftsEnabled;
	LWLockId	ControlLock;
	int			ftsSyncCount;
	int			ftsAction;
	bool		ftsReadOnlyFlag;

	struct FtsQDMirrorInfo	ftsQDMirror;

	int			ftsSegDBTransientCount;
	struct FtsSegDBState ftsSegDBTransientState[FTS_MAX_TRANSIENT_STATE];
}	FtsControlBlock;

typedef enum
{
	FTS_NONE,					/* disable fts */
	FTS_RECONNECT,				/* re-connect */
	FTS_CONTINUE,				/* updatable */
	FTS_READONLY,				/* read only */
	FTS_SHUTDOWN				/* shutdown */
}	FtsAction;

extern bool *ftsEnabled;
extern int *ftsAction;

#define isFTSEnabled() (ftsEnabled != NULL && *ftsEnabled)
/* #define enableMirroring() (*ftsEnabled = (*ftsAction!=FTS_NONE)?true:false) */
#define disableFTS() (*ftsEnabled = false)

extern int	FtsShmemSize(void);
extern void FtsShmemInit(void);
extern void CondSetMPPReadonly(void);
extern bool FtsReConnectDB(void *);
extern bool FtsTestConnection(CdbComponentDatabaseInfo *db_to_test);
extern bool FtsReConfigureMPP(void);
extern void FtsHandleNetFailure(SegmentDatabaseDescriptor **, int);
extern bool FtsHandleGangConnectionFailure(SegmentDatabaseDescriptor *, int);
extern void markStandbyStatus(int dbid, bool state);
extern void markSegDBStatus(int, bool);
extern const char *show_gp_set_read_only(void);
extern bool assign_gp_set_read_only(bool, bool, GucSource);
extern int	convertFaultActionValue(char *);
extern bool verifyFtsSyncCount(void);
extern void FtsCondSetTxnReadOnly(bool *);
extern void disableQDMirroring(void);
extern void unexpectedErrorQDMirroring(void);
extern CdbComponentDatabases *applyTransientState(CdbComponentDatabases *);
extern void FtsEvaluateTransientState(void);
extern bool isFtsReadOnlySet(void);

extern void FtsGetQDMirrorInfo(char **hostname, uint16 *port);
void FtsDumpQDMirrorInfo(void);
extern bool isMirrorQDConfigurationChecked(void);
extern bool isQDMirroringPendingCatchup(void);
extern void FtsSetQDMirroring(struct CdbComponentDatabaseInfo *dbinfo);
extern void FtsSetNoQDMirroring(void);
extern void FtsQDMirroringCatchup(XLogRecPtr *flushedLocation);
extern void FtsCheckReadyToSend(bool *ready, bool *haveNewCheckpointLocation, XLogRecPtr *newCheckpointLocation);
extern void FtsQDMirroringNewCheckpointLoc(XLogRecPtr *newCheckpointLocation);

#endif   /* CDBFTS_H */
