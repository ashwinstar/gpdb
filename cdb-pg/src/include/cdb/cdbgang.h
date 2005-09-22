#ifndef _CDBGANG_H_
#define _CDBGANG_H_

#include "nodes/primnodes.h"
#include "cdb/cdbutil.h"

/* forward declare segment_database_descriptor so everybody doesn't need to pull in it's definition */
typedef struct segment_database_descriptor *SegmentDatabaseDescriptorPtr;


/*
 * A gang represents a single worker on each connected segDB
 *
 */

typedef struct Gang
{
	GangType	type;
	int			size; 			  /* segment_count or segdb_count ? */
	int			segdb_count;
	bool		all_valid_segdbs_connected;
	SegmentDatabaseDescriptorPtr db_descriptors;		/* Array of QEs/segDBs
														 * that make up this
														 * gang */
}	Gang;

extern Gang *allocateGang(GangType type, int size, int content);

extern List *getCdbProcessList(Gang * gang);

extern List *getCdbProcessesForQD(int isPrimary);

extern void freeGangs(void);

extern void disconnectAndDestroyAllGangs(void);

extern bool reconnectSegDB(void *);

extern void detectFailedConnections(void);

extern CdbComponentDatabases * getComponentDatabases(void);

extern bool isGangExisted(void);


#endif   /* _CDBGANG_H_ */
