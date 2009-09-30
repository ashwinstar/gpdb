/*-------------------------------------------------------------------------
 *
 * cdbaocsam.h
 *	  append-only columnar relation access method definitions.
 *
 * Portions Copyright (c) 2009, Greenplum Inc.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDB_AOCSAM_H
#define CDB_AOCSAM_H

#include "access/relscan.h"
#include "access/sdir.h"
#include "access/tupmacs.h"
#include "access/xlogutils.h"
#include "executor/tuptable.h"
#include "nodes/primnodes.h"
#include "storage/block.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/tqual.h"

/*
 * AppendOnlyInsertDescData is used for inserting data into append-only
 * relations. It serves an equivalent purpose as AppendOnlyScanDescData
 * (relscan.h) only that the later is used for scanning append-only 
 * relations. 
 */
struct DatumStream;
struct AOCSFileSegInfo;

typedef struct AOCSInsertDescData
{
	Relation	aoi_rel;
	int64		insertCount;
	int64		varblockCount;
	int32		cur_segno;

    CompressorType  compType;
	int32 		compLevel;
    int32       blocksz;

	struct DatumStream     **ds;
} AOCSInsertDescData;

typedef AOCSInsertDescData *AOCSInsertDesc;

/*
 * used for scan of append only relations using BufferedRead and VarBlocks
 */
typedef struct AOCSScanDescData
{
	/* scan parameters */
	Relation	aos_rel;			/* target relation descriptor */
	Index       aos_scanrelid;                  /* index */
	int		    aos_nkeys;			/* number of scan keys */
	ScanKey		aos_key;			/* array of scan key descriptors */

    int         total_seg;
    int         cur_seg;

    CompressorType  compType;
    int32 		compLevel;
    int32       blocksz;

    struct AOCSFileSegInfo **seginfo;
    struct DatumStream     **ds;
    bool            *proj;

    /* synthetic system attributes */
    ItemPointerData cdb_fake_ctid;
    int64 total_row;
    int64 cur_seg_row;

}	AOCSScanDescData;

typedef AOCSScanDescData *AOCSScanDesc;


/* ----------------
 *		function prototypes for appendonly access method
 * ----------------
 */

extern AOCSScanDesc aocs_beginscan(Relation relation, Snapshot snapshot, int nkeys, ScanKey key, bool *proj);
extern void aocs_rescan(AOCSScanDesc scan, ScanKey key);
extern void aocs_endscan(AOCSScanDesc scan);
extern void aocs_getnext(AOCSScanDesc scan, ScanDirection direction, TupleTableSlot *slot);
extern AOCSInsertDesc aocs_insert_init(Relation rel, int segno);
extern Oid aocs_insert_values(AOCSInsertDesc idesc, Datum *d, bool *null);
static inline Oid aocs_insert(AOCSInsertDesc idesc, TupleTableSlot *slot)
{
        slot_getallattrs(slot);
        return aocs_insert_values(idesc, slot_get_values(slot), slot_get_isnull(slot));
}
extern void aocs_insert_finish(AOCSInsertDesc idesc);

#endif   /* AOCSAM_H */
