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

typedef struct FtsControlBlock
{
    bool        ftsEnabled;
    LWLockId    ControlLock;
    int         ftsSyncCount;
    int         ftsAction;
    bool        ftsReadOnlyFlag;
} FtsControlBlock;


typedef enum
{
    FTS_NONE,            /* disable fts */
	FTS_RECONNECT,       /* re-connect */
	FTS_CONTINUE,		/* updatable */
    FTS_READONLY,        /* read only */
    FTS_SHUTDOWN		    /* shutdown */
} FtsAction;
	

extern int FtsShmemSize(void);
extern void FtsShmemInit(void);
extern void CondSetMPPReadonly(void);
extern bool FtsReConnectDB(void *);
extern bool FtsTestConnection(SegmentDatabaseDescriptor *);
extern bool FtsReConfigureMPP(void);
extern void FtsHandleNetFailure( SegmentDatabaseDescriptor **, int);
extern void FtsHandleGangConnectionFailure(SegmentDatabaseDescriptor *, int);
extern void markSegDBStatus( int, bool);
extern const char * show_cdb_set_read_only(void);
extern bool assign_cdb_set_read_only( bool, bool, GucSource);
extern int convertFaultActionValue(char *);
extern bool verifyFtsSyncCount(void);
extern void enableMirroring(void);
extern void FtsCondSetTxnReadOnly(bool *);
extern bool isMirroringEnabled(void);

#endif /* CDBFTS_H */
