/*-------------------------------------------------------------------------
 *
 * cdbgang.h
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef _CDBGANG_H_
#define _CDBGANG_H_

#include "cdb/cdbutil.h"
#include "executor/execdesc.h"

struct Port;                    /* #include "libpq/libpq-be.h" */
struct QueryDesc;               /* #include "executor/execdesc.h" */


typedef struct DispatchAgentSet
{
	int		size;
	int		DAset_id;
	struct	SegmentDatabaseDescriptor *db_descriptors;
} DispatchAgentSet;

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
	bool		active;
	char		*portal_name; /* the named portal that owns this gang, NULL if none */
	struct DispatchAgentSet * dispatchagents;		/* NULL if gang is directly connected */
	struct SegmentDatabaseDescriptor *db_descriptors;	/* Array of QEs/segDBs
														 * that make up this
														 * gang */
} Gang;

typedef struct GpSegmentConnectionInfo
{
	char	hostname[NAMEDATALEN];
	//char	hostip[32];			// Do we need this?
	int4	port;
	int2	segindex;
	bool	isprimary;
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
extern Gang *allocatePrimaryWriterGang(void);
extern Gang *allocateMirrorWriterGang(void);

extern List *getCdbProcessList(Gang *gang, int sliceIndex);

extern bool dispatchagentsOk(DispatchAgentSet * das);

extern bool gangOK(Gang * gp);

extern List *getCdbProcessesForQD(int isPrimary);

extern void freeGangsForPortal(char *portal_name);

extern void disconnectAndDestroyAllGangs(void);

extern List * getAllReaderGangs(void);

extern void detectFailedConnections(void);

extern CdbComponentDatabases *getComponentDatabases(void);

extern bool gangsExist(void);

struct SegmentDatabaseDescriptor *
getSegmentDescriptorFromGang(const Gang *gp, int seg);

bool
reconnectSegDB(struct SegmentDatabaseDescriptor * failedDB);

extern void
SetSeqServerInfo(struct QueryDesc *queryDesc);

extern GpSegmentConnectionResponse *
createGangFragment(GpSegmentConnectionInfoSet *conninfo, int * outsize);

extern Gang * 
findGangById(int gang_id);

extern bool
cleanupGang(Gang * gp);

extern void
cleanupIdleReaderGangs(void);

extern void
cleanupPortalGangs(Portal portal);

extern void
disconnectAndDestroyGangById(int gang_id);

extern void disconnectAndDestroyDispatchAgents(struct DispatchAgentSet * das);

extern int numDispatchAgents(void);
extern int largestGangsize(void);
extern struct DispatchAgentSet * getUnamedPortalDispatchAgents(void);

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
