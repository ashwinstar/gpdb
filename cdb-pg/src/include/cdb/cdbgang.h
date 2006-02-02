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
	bool		allocated;
	bool		active;
	struct SegmentDatabaseDescriptor *db_descriptors;	/* Array of QEs/segDBs
														 * that make up this
														 * gang */
} Gang;

extern Gang *allocateGang(GangType type, int size, int content);

extern List *getCdbProcessList(Gang *gang);

extern List *getCdbProcessesForQD(int isPrimary);

extern void freeGangs(void);

extern void disconnectAndDestroyAllGangs(void);

extern List * getAllReaderGangs(void);

extern void detectFailedConnections(void);

extern CdbComponentDatabases *getComponentDatabases(void);

extern bool gangsExist(void);

struct SegmentDatabaseDescriptor *
getSegmentDescriptorFromGang(const Gang *gp, int seg);

bool
reconnectSegDB(struct SegmentDatabaseDescriptor * failedDB);

#endif   /* _CDBGANG_H_ */
