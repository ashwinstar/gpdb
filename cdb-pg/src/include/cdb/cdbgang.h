/*-------------------------------------------------------------------------
 *
 * cdbgang.h
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef _CDBGANG_H_
#define _CDBGANG_H_

#include "postgres.h"
#include "cdb/cdbutil.h"
#include "executor/execdesc.h"
#include <pthread.h>

struct Port;                    /* #include "libpq/libpq-be.h" */
struct QueryDesc;               /* #include "executor/execdesc.h" */

/*
 * A gang represents a single worker on each connected segDB
 *
 */

typedef struct Gang
{
	GangType	type;
	int			gang_id;
	int			size;			/* segment_count or segdb_count ? */
	int			segdb_count;
	bool		all_valid_segdbs_connected;
	bool		allocated;
	bool		active; /* ?? */

	/* MPP-6253: on *writer* gangs keep track of dispatcher use
	 * (reader gangs already track this properly, since they get
	 * allocated from a list of available gangs.*/
	bool		dispatcherActive; 

	/* the named portal that owns this gang, NULL if none */
	char		*portal_name;

	/* Array of QEs/segDBs that make up this gang */
	struct SegmentDatabaseDescriptor *db_descriptors;	
} Gang;

typedef struct GpSegmentConnectionInfo
{
	char	hostname[NAMEDATALEN];
	//char	hostip[32];			// Do we need this?
	int4	port;
	int2	segindex;
} GpSegmentConnectionInfo;

typedef struct GpSegmentConnectionInfoSet
{
	int		nsegs;
	GangType type;
	int		gang_id;
	int		content; // if 1 gang
	struct GpSegmentConnectionInfo seginfo[1];
} GpSegmentConnectionInfoSet;

typedef struct GpSegmentConnectionResponse
{
	int4	motionListener;
	int4	pid;
	//char	hostip[32];			// Do we need this?
} GpSegmentConnectionResponse;

extern Gang *allocateGang(GangType type, int size, int content, char *portal_name);
extern Gang *allocateWriterGang(void);

struct DirectDispatchInfo;
extern List *getCdbProcessList(Gang *gang, int sliceIndex, struct DirectDispatchInfo *directDispatch);

extern bool gangOK(Gang * gp);

extern List *getCdbProcessesForQD(int isPrimary);

extern void freeGangsForPortal(char *portal_name);

extern void disconnectAndDestroyAllGangs(void);

extern void CheckForResetSession(void);

extern List * getAllReaderGangs(void);

extern void detectFailedConnections(void);

extern CdbComponentDatabases *getComponentDatabases(void);

extern bool gangsExist(void);

struct SegmentDatabaseDescriptor *
getSegmentDescriptorFromGang(const Gang *gp, int seg);

bool
reconnectSegDB(struct SegmentDatabaseDescriptor * failedDB);

extern Gang * 
findGangById(int gang_id);

extern bool
cleanupGang(Gang * gp);


/*
 * cleanupIdleReaderGangs() and cleanupAllIdleGangs().
 *
 * These two routines are used when a session has been idle for a while (waiting for the
 * client to send us SQL to execute).  The idea is to consume less resources while sitting idle.
 *
 * Only call these from an idle session.
 */
extern void
cleanupIdleReaderGangs(void);

extern void
cleanupAllIdleGangs(void);

/**/

extern void
cleanupPortalGangs(Portal portal);

extern void
disconnectAndDestroyGangById(int gang_id);

extern int largestGangsize(void);

extern int gp_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg, const char *caller);
/*
 * cdbgang_parse_gpqeid_params
 *
 * Called very early in backend initialization, to interpret the "gpqeid"
 * parameter value that a qExec receives from its qDisp.
 *
 * At this point, client authentication has not been done; the backend
 * command line options have not been processed; GUCs have the settings
 * inherited from the postmaster; etc; so don't try to do too much in here.
 */
void
cdbgang_parse_gpqeid_params(struct Port *port, const char* gpqeid_value);

void
cdbgang_parse_gpqdid_params(struct Port *port, const char* gpqdid_value);

void
cdbgang_parse_gpdaid_params(struct Port *port, const char* gpdaid_value);
#endif   /* _CDBGANG_H_ */
