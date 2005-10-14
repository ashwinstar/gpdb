/*-------------------------------------------------------------------------
 *
 * cdbgang.h
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef _CDBGANG_H_
#define _CDBGANG_H_

#include "nodes/primnodes.h"
#include "cdb/cdbutil.h"


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
	struct SegmentDatabaseDescriptor *db_descriptors;	/* Array of QEs/segDBs
														 * that make up this
														 * gang */
	struct CdbDispatchResults *dispatchResults; /* outcome of execution */
}	Gang;

extern Gang *allocateGang(GangType type, int size, int content);

extern List *getCdbProcessList(Gang * gang);

extern List *getCdbProcessesForQD(int isPrimary);

extern void freeGangs(void);

extern void disconnectAndDestroyAllGangs(void);

extern void detectFailedConnections(void);

extern CdbComponentDatabases *getComponentDatabases(void);

extern bool isGangExisted(void);

struct SegmentDatabaseDescriptor *
			getSegmentDescriptorFromGang(const Gang * gp, int seg);

bool
			reconnectSegDB(struct SegmentDatabaseDescriptor * failedDB);

#endif   /* _CDBGANG_H_ */
