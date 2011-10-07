/*
 * Datumstream
 * 	Copyright (c) 2008, Greenplum Inc.
 */

#ifndef DATUM_STREAM_H
#define DATUM_STREAM_H

#include "catalog/pg_attribute.h"

/*
 * Datum Stream Block.
 * 16 bytes header.  Followed by data.
 */
typedef struct DatumStreamBlock
{
    int16 version; 	/* version number */
    int16 flags;	/* some flags */
    int16 ndatum;	/* number of datum, including null */
    int16 unused;   /* unused */
    int32 nullsz; 	/* size nullbitmaps */
    int32 sz;	    /* logical data size, not including header, nullbitmap, and padding */
} DatumStreamBlock;

/* Flags */
enum {
    DSB_HAS_NULLBITMAP = 1,
};

/*
 * Magic number.  Max number of datum in on block.
 * MUST fit in 15 bit, or we overflow ndatum(int16), defined
 * in DatumStreamBlock.
 *
 * It could be uint16 (64K), but the negative numbers have not
 * be carefully debugged.
 *
 * This number also needs to be not greater than
 * AOBlockHeader_MaxRowCount. Since AOBlockHeader_MaxRowCount is
 * 14-bit, we just use it here.
 */
#define MAXDATUM_PER_AOCS_BLOCK AOBlockHeader_MaxRowCount

typedef struct DatumStream
{
    /* Info determined by schema */
    int32 datumlen;		    /* Datum length */
    int32 typid;            /* type id */
    char  align;		    /* Align */
    bool  byval;            /* if it is a by value type */

    int32 compress_option; /* Data type specific compression */

    /* Acc max limit */
    int32 maxsz;		    /* max size */
    int32 maxdatasz;        /* max usable data size */

    /* reader or writer ? */
    bool is_reader;

    /* AO Storage */
    bool need_close_file;
    AppendOnlyStorageAttributes ao_attr;
    AppendOnlyStorageRead *ao_read;
    AppendOnlyStorageWrite *ao_write;

	/* The info for the block that is currently read. */
	int64 blockFirstRowNum;
	int64 blockFileOffset;
	int blockRowCount;


    /* current pointer and nullbit */
    int32 nth;		    /* Nth datum in the block */
    bool  has_null;		/* if we have null at all */
    char* nullp;		/* pointer to current null bitmap char */
    char  nullbit;		/* current null bit */
    char* datump;		/* poitner to datum */

    DatumStreamBlock *block;    /* Current read block */

    /* Buffers */
    MemoryContext memctxt;
    char *npbuf;
    int  npbufsz;
    char *dpbuf;
    int  dpbufsz;
    char *lobbuf;
    int  lobbufsz;

    /* EOF of current file */
    int64 eof;
    int64 eofUncompress;
} DatumStream;

/*
 * A structure contains the state when fetching rows
 * through datum stream.
 */
typedef struct DatumStreamFetchDescData
{
	DatumStream *datumStream;

	CurrentSegmentFile currentSegmentFile;

	int64 scanNextFileOffset;
	int64 scanNextRowNum;

	int64 scanAfterFileOffset;
	int64 scanLastRowNum;

	CurrentBlock currentBlock;

} DatumStreamFetchDescData;

typedef DatumStreamFetchDescData *DatumStreamFetchDesc;

/* Stream access method */
extern int datumstream_get(DatumStream *ds, Datum *datum, bool *null);
extern int datumstream_advance(DatumStream *ds);
extern int datumstream_put(DatumStream *ds, Datum d, bool n);

/* ctor and dtor */
extern DatumStream *create_datumstream(char *compName, int32 compLv, int32 maxsz, size_t (*desiredCompSizeFunc)(size_t input),
		AORelationVersion version, Form_pg_attribute attr, char *relname, bool isreader);
extern void datumstream_open_file(
		DatumStream *ds,
		char *fn,
		int64 eof,
		int64 eofUncompressed,
		RelFileNode relFileNode,
		int32	segmentFileNum);
extern void datumstream_close_file(DatumStream *ds);
extern void destroy_datumstream(DatumStream *ds);

/* Read and Write op */
extern int64 datumstream_write_block(DatumStream *ds);
extern int64 datumstream_write_lob(DatumStream *ds, Datum d);
extern int datumstream_read_block(DatumStream *ds);
extern void datumstream_find(DatumStream *datumStream,
							 int32 rowNumInBlock);
extern void datumstream_rewind_block(DatumStream *datumStream);
extern bool datumstream_find_block(DatumStream *datumStream,
								   DatumStreamFetchDesc datumStreamFetchDesc,
								   int64 rowNum);

#endif /* DATUM_STREAM_H */
