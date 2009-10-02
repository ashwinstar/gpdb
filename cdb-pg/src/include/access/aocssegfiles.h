/* 
 * aocssegfiles.h
 *      AOCS segment files
 * Copyright (c) 2009, Greenplum INC.
 */

#ifndef AOCS_SEGFILES_H
#define AOCS_SEGFILES_H

#define Natts_pg_aocsseg 4
#define Anum_pg_aocs_segno 1
#define Anum_pg_aocs_tupcount 2
#define Anum_pg_aocs_varblockcount 3
#define Anum_pg_aocs_vpinfo 4

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

typedef struct AOCSFileSegInfo
{
        int32 segno;
        int64 tupcount;
        int64 varblockcount;

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

extern AOCSFileSegInfo *GetAOCSFileSegInfoForRead(Relation prel, int32 segno);
extern AOCSFileSegInfo *GetAOCSFileSegInfoForWrite(Relation prel, int32 segno);
extern AOCSFileSegInfo *GetAOCSFileSegInfoForVacuum(Relation prel, int32 segno);

extern AOCSFileSegInfo **GetAllAOCSFileSegInfo(Relation prel, Snapshot snapshot, int *totalseg);

extern int64 GetAOCSTotalBytes(Relation parentrel);

extern void InsertInitialAOCSFileSegInfo(Relation parentrel, int32 segno, int32 nvp); 
extern void UpdateAOCSFileSegInfo(struct AOCSInsertDescData *desc);
extern void AOCSFileSegInfoAddCount(Relation prel, int32 segno, int64 tupadded, int64 varblockadded); 

extern Datum gp_update_aocol_master_stats_internal(Relation parentrel);
extern Datum aocol_compression_ratio_internal(Relation parentrel);

#endif
