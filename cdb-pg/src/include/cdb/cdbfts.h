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

#define FTS_MAX_TRANSIENT_STATE 100

typedef struct FtsControlBlock
{
	bool		ftsEnabled;
	LWLockId	ControlLock;
	int			ftsSyncCount;
	int			ftsAction;
	bool		ftsReadOnlyFlag;
	bool		ftsQDMirroring;
	int			ftsSegDBTransientCount;
	uint16		ftsSegDBTransientState[FTS_MAX_TRANSIENT_STATE];
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

#define isFTSEnabled() (*ftsEnabled)
/* #define enableMirroring() (*ftsEnabled = (*ftsAction!=FTS_NONE)?true:false) */
#define disableFTS() (*ftsEnabled = false)

extern int	FtsShmemSize(void);
extern void FtsShmemInit(void);
extern void CondSetMPPReadonly(void);
extern bool FtsReConnectDB(void *);
extern bool FtsTestConnection(CdbComponentDatabaseInfo *, char *);
extern bool FtsReConfigureMPP(void);
extern void FtsHandleNetFailure(SegmentDatabaseDescriptor **, int);
extern bool FtsHandleGangConnectionFailure(SegmentDatabaseDescriptor *, int);
extern void markSegDBStatus(int, bool);
extern const char *show_mpp_set_read_only(void);
extern bool assign_mpp_set_read_only(bool, bool, GucSource);
extern int	convertFaultActionValue(char *);
extern bool verifyFtsSyncCount(void);
extern void FtsCondSetTxnReadOnly(bool *);
extern void disableQDMirroring(void);
extern CdbComponentDatabases *applyTransientState(CdbComponentDatabases *);
extern void FtsEvaluateTransientState(void);

#endif   /* CDBFTS_H */
