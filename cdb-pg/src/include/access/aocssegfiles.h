/* 
 * aocssegfiles.h
 *      AOCS segment files
 * Copyright (c) 2009, Greenplum INC.
 */

#ifndef AOCS_SEGFILES_H
#define AOCS_SEGFILES_H

#define Natts_pg_aocsseg 5
#define Anum_pg_aocs_segno 1
#define Anum_pg_aocs_tupcount 2
#define Anum_pg_aocs_varblockcount 3
#define Anum_pg_aocs_vpinfo 4


/*
 * pg_aocsseg_nnnnnn table values for FormData_pg_attribute.
 *
 * [Similar examples are Schema_pg_type, Schema_pg_proc, Schema_pg_attribute, etc, in
 *  pg_attribute.h]
 */
#define Schema_pg_aocsseg \
{ -1, {"segno"}, 				23, -1, 4, 1, 0, -1, -1, true, 'p', 'i', false, false, false, true, 0 }, \
{ -1, {"tupcount"},				20, -1, 8, 2, 0, -1, -1, true, 'p', 'd', false, false, false, true, 0 }, \
{ -1, {"varblockcount"},		20, -1, 8, 3, 0, -1, -1, true, 'p', 'd', false, false, false, true, 0 }, \
{ -1, {"vpinfo"},				1562, -1, -1, 4, 0, -1, -1, false, 'x', 'i', false, false, false, true, 0 }

/*
 * pg_aoseg_nnnnnn table values for FormData_pg_class.
 */
#define Class_pg_aocsseg \
  {"pg_appendonly"}, PG_CATALOG_NAMESPACE, -1, BOOTSTRAP_SUPERUSERID, 0, \
               -1, DEFAULTTABLESPACE_OID, \
               25, 10000, 0, 0, 0, 0, false, false, RELKIND_RELATION, RELSTORAGE_HEAP, Natts_pg_aocsseg, \
               0, 0, 0, 0, 0, false, false, false, false, FirstNormalTransactionId, {0}, {{{'\0','\0','\0','\0'},{'\0'}}}


typedef struct AOCSVPInfoEntry
{
        int64 eof;
        int64 eof_uncompressed;
} AOCSVPInfoEntry;
        
typedef struct AOCSVPInfo
{
        /* total len.  Have to be the very first */ 
        int32 _len;     
        int32 version;
        int32 nEntry;

        /* Var len array */
        AOCSVPInfoEntry entry[1];
} AOCSVPInfo;

static inline int aocs_vpinfo_size(int nvp)
{
        return offsetof(AOCSVPInfo, entry) + 
               sizeof(AOCSVPInfoEntry) * nvp;
}
static inline AOCSVPInfo *create_aocs_vpinfo(int nvp)
{
        AOCSVPInfo *vpinfo = palloc0(aocs_vpinfo_size(nvp));
        SET_VARSIZE(vpinfo, aocs_vpinfo_size(nvp));
        vpinfo->nEntry = nvp;
        return vpinfo;
}

/*
 * Descriptor of a single AO Col relation file segment.
 *
 * Note that the first three variables should be the same as
 * the AO Row relation file segment (see FileSegInfo). This is
 * implicitly used by the block directory to obtain those info
 * using the same structure -- FileSegInfo.
 */
typedef struct AOCSFileSegInfo
{
	int32 segno;
	int64 tupcount;
	int64 varblockcount;
	ItemPointerData sequence_tid;

	/* Must be last */
	AOCSVPInfo vpinfo;
} AOCSFileSegInfo;

static inline int aocsfileseginfo_size(int nvp)
{
        return offsetof(AOCSFileSegInfo, vpinfo) + 
                aocs_vpinfo_size(nvp);
}

static inline AOCSVPInfoEntry *getAOCSVPEntry(AOCSFileSegInfo *psinfo, int vp)
{
        Assert(vp < psinfo->vpinfo.nEntry);
        return &(psinfo->vpinfo.entry[vp]);
}

struct AOCSInsertDescData;

extern AOCSFileSegInfo *GetAOCSFileSegInfoForRead(Relation prel, AppendOnlyEntry *aoEntry, int32 segno);
extern AOCSFileSegInfo *GetAOCSFileSegInfoForWrite(Relation prel, AppendOnlyEntry *aoEntry, int32 segno);
extern AOCSFileSegInfo *GetAOCSFileSegInfoForVacuum(Relation prel, AppendOnlyEntry *aoEntry, int32 segno);

extern AOCSFileSegInfo **GetAllAOCSFileSegInfo(Relation prel, AppendOnlyEntry *aoEntry, Snapshot snapshot, int *totalseg);
extern AOCSFileSegInfo **GetAllAOCSFileSegInfo_pg_aocsseg_rel(int numOfColumsn, char *relationName, AppendOnlyEntry *aoEntry, Relation pg_aocsseg_rel, Snapshot snapshot, int32 *totalseg);

extern int64 GetAOCSTotalBytes(Relation parentrel);

extern void InsertInitialAOCSFileSegInfo(Oid segrelid, int32 segno, int32 nvp); 
extern void UpdateAOCSFileSegInfo(struct AOCSInsertDescData *desc);
extern void AOCSFileSegInfoAddCount(Relation prel, AppendOnlyEntry *aoEntry, int32 segno, int64 tupadded, int64 varblockadded); 

extern Datum gp_update_aocol_master_stats_internal(Relation parentrel);
extern Datum aocol_compression_ratio_internal(Relation parentrel);

#endif
