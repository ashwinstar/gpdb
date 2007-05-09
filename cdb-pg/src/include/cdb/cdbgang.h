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

/*
 * A gang represents a single worker on each connected segDB
 *
 */

typedef struct Gang
{
	GangType	type;
	int			size;			/* segment_count or segdb_count ? */
	int			segdb_count;
	bool		all_valid_segdbs_connected;
	bool		allocated;
	bool		active;
	char		*portal_name; /* the named portal that owns this gang, NULL if none */
	struct SegmentDatabaseDescriptor *db_descriptors;	/* Array of QEs/segDBs
														 * that make up this
														 * gang */
} Gang;

extern Gang *allocateGang(GangType type, int size, int content, char *portal_name);

extern List *getCdbProcessList(Gang *gang, int sliceIndex);

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
#endif   /* _CDBGANG_H_ */
