/*
 * Datumstream
 * 	Copyright (c) 2008, Greenplum Inc.
 */

#ifndef DATUM_STREAM_H
#define DATUM_STREAM_H

#include "catalog/pg_attribute.h"
#include "storage/fd.h"
#include "storage/gp_compress.h"

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
    int32 nullsz2;  /* size of compressed nullbitmaps */
    int32 sz;	/* logical data size, not including header, nullbitmap, and padding */
    int32 sz2;	/* physical data size, not including header, nullbitmap, and padding */
} DatumStreamBlock;

/* Flags */
enum {
    DSB_HAS_NULLBITMAP = 1,
};

typedef struct DatumStream
{
    /* Info determined by schema */
    int32 datumlen;		/* Datum length */
    int32 typid;            /* type id */
    char  align;		/* Align */

    /* Acc max limit */
    int32 maxsz;		/* max size */

    /* Compression */
    CompressorType compType;
    int32   compLevel;
    int32 compress_option;  /* compress option: data type specific */	

    /* reader or writer ? */
    bool is_reader;	

    /* Underlying backing file */
    File f;
    char *path;
    int64 eof;
    int64 eof_uncompressed;
    int64 reof;

    /* Current block that we are reading */
    DatumStreamBlock block; 

    /* current pointer and nullbit */
    int32 nth;		/* Nth datum in the block */
    bool  has_null;		/* if we have null at all */
    char* nullp;		/* pointer to current null bitmap char */
    char  nullbit;		/* current null bit */ 
    char* datump;		/* poitner to datum */

    /* Buffers */
    char *npbuf; 
    int32 npbufsz;
    char *dpbuf; 
    int32 dpbufsz;

    /* compressed buffers */
    char *npzbuf;
    int32 npzbufsz;
    char *dpzbuf;
    int32 dpzbufsz;

    /* quicklz scratch */
    int32 qlz_scratchsz;
    char *qlz_scratch;
} DatumStream;

/* Stream access method */
extern int datumstream_get(DatumStream *ds, Datum *datum, bool *null);
extern int datumstream_advance(DatumStream *ds);
extern int datumstream_put(DatumStream *ds, Datum d, bool n);

/* ctor and dtor */
extern DatumStream *create_datumstream(CompressorType compType, int32 compLv, int32 maxsz, 
        Form_pg_attribute attr, char *fn, bool isreader);
extern void destroy_datumstream(DatumStream *ds);
extern int64 datumstream_write_block(DatumStream *ds);
extern int64 datumstream_write_lob(DatumStream *ds, Datum d);
extern int datumstream_read_block(DatumStream *ds);

#endif /* DATUM_STREAM_H */
