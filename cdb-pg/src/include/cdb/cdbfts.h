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

struct FtsQDMirrorInfo
{
	bool		enabled;
	/* stored copies of QD parameters, required so that bg-writer
	 * process knows how to connect */
	char		name[256];
	uint16		port;
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
extern bool FtsTestConnection(CdbComponentDatabaseInfo *db_to_test, char *user, char *db);
extern bool FtsReConfigureMPP(void);
extern void FtsHandleNetFailure(SegmentDatabaseDescriptor **, int);
extern bool FtsHandleGangConnectionFailure(SegmentDatabaseDescriptor *, int);
extern void markSegDBStatus(int, bool);
extern const char *show_gp_set_read_only(void);
extern bool assign_gp_set_read_only(bool, bool, GucSource);
extern int	convertFaultActionValue(char *);
extern bool verifyFtsSyncCount(void);
extern void FtsCondSetTxnReadOnly(bool *);
extern void disableQDMirroring(void);
extern CdbComponentDatabases *applyTransientState(CdbComponentDatabases *);
extern void FtsEvaluateTransientState(void);
extern bool isFtsReadOnlySet(void);

extern void FtsUpdateQDMirrorInfo(struct CdbComponentDatabaseInfo *dbinfo);
extern void FtsGetQDMirrorInfo(char **hostname, uint16 *port);
void FtsDumpQDMirrorInfo(void);

#endif   /* CDBFTS_H */
