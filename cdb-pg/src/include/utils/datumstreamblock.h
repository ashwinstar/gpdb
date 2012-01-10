/*
 * DatumStreamBlock
 * 	Copyright (c) 201, EMC Inc.
 */

#ifndef DATUMSTREAMBLOCK_H
#define DATUMSTREAMBLOCK_H

#include "catalog/pg_attribute.h"

typedef enum DatumStreamVersion
{
	DatumStreamVersion_Original =  0,		/* first valid version */
	DatumStreamVersion_Dense = 1,	
											/* Version used for denser header (higher
											 * row count) and RLE_TYPE compression
											 * done by this module.
											 */

	MaxDatumStreamVersion                    /* must always be last */
} DatumStreamVersion;

/* 
 * Datum Stream Block (Original).
 * 16 bytes header.  Followed by data.
 */
typedef struct DatumStreamBlock_Orig
{
    int16 version; 	/* version number */
    int16 flags;	/* some flags */
    int16 ndatum;	/* number of datum, including null */
    int16 unused;   /* unused */
    int32 nullsz; 	/* size nullbitmaps */
    int32 sz;	    /* logical data size, not including header, nullbitmap, and padding */
} DatumStreamBlock_Orig;

/* 
 * Datum Stream Block (Dense).
 *
 * Dense format uses an Append-Only Storage Block header that has a larger RowCount
 * field so we can store more NULL and/or represent more RLE_TYPE compressed items.
 *
 * 16 bytes header.
 */
typedef struct DatumStreamBlock_Dense
{
	struct orig_4_bytes
	{
		int16 version;	/* version number */
		int16 flags;	/* some flags */
	} orig_4_bytes;

	int32 	logical_row_count;	
						/*
						  * Number of physical datum PLUS number of NULLs; The logical
						  * count of rows. 
						  */

	int32	physical_datum_count;	
						/* Number of physical datum.  May be 0 if block is all NULLs. */

	int32	physical_data_size;	
						/* 
						  * Total data size of all physical datums in block.  Does not
						  * include header(s), bitmap(s), padding between headers and
						  * datum area, etc.
						  */

} DatumStreamBlock_Dense;

/* 
 * Datum Stream Block extension to DatumStreamBlock_Dense with RLE_TYPE compression data.
 * 16 bytes more.
 */
typedef struct DatumStreamBlock_Rle_Extension
{
	int32 	norepeats_null_bitmap_count;
						/*
						 * For non-RLE_TYPE compression, if there is a NULL bit-map,
						 * it will have row_count (see above) number of bits.
						 *
						 * However, for RLE_TYPE compression, we do not have a bunch
						 * of NULL bit-map bits for a repeated item.  We just use one bit
						 * (a 0, of course, indicating the repeated item is not NULL).  So,
						 * the NULL bit-map count is different than row_count.
						 */
	
	int32 	compress_bitmap_count;
						/*
						 * Number of bits in the compress bit-map.
						 */

	int32 	repeatcounts_count;
						/*
						 * Total number of repeated items.
						 *
						 * Also, the count of the ON bits in the compress bit-map.
						 */

	int32 	repeatcounts_size;
						/*
						 * Total size of the repeat counts array, when you account for
						 * the different 1, 2, 3, and 4 byte encoding size of each count.
						 */
} DatumStreamBlock_Rle_Extension;


/* Flags */
enum {
    DSB_HAS_NULLBITMAP = 		0x1,
	DSB_HAS_RLE_COMPRESSION = 	0x2,
};

typedef struct DatumStreamBitMapWrite
{
	uint8	*buffer;
	int32	bufferSize;

	uint8 	byteBit;
	uint8 	*bytePointer;
	int32 	bitOnCount;
	int32	bitCount;
} DatumStreamBitMapWrite;
	
typedef struct DatumStreamBitMapRead
{
	uint8	*buffer;

	uint8	byteBit;
	uint8	*bytePointer;
	int32	readBitOnCount;
	int32	bitCount;
	int32	bitPosition;
} DatumStreamBitMapRead;

typedef struct DatumStreamTypeInfo
{
    /* Info determined by schema */
    int32 datumlen;		    /* Datum length */
    int32 typid;            /* type id */
    char  align;		    /* Align */
    bool  byval;            /* if it is a by value type */ 
} DatumStreamTypeInfo;

#define MAXREPEAT_COUNT 0x3FFFFFFF

#define DatumStreamBlockWrite_Eyecatcher "DBW"
#define DatumStreamBlockWrite_EyecatcherLen 4

typedef struct DatumStreamBlockWrite
{
	char		eyecatcher[DatumStreamBlockWrite_EyecatcherLen];	
								/* 
								 * Used to validate this is a DatumStreamBlockWrite
								 * reference.  3 characters plus NUL.
								 */

	DatumStreamTypeInfo	*typeInfo;

	/*
	 * Version of datum stream  block format -- original or RLE_TYPE,
	 */
	DatumStreamVersion	datumStreamVersion;

	bool	rle_want_compression;

	int32	initialMaxDatumPerBlock;
	int32	maxDatumPerBlock;

	int32	maxDataBlockSize;

	int		(*errdetailCallback) (void *errdetailArg);
	void	*errdetailArg;
	int		(*errcontextCallback) (void *errcontextArg);
	void	*errcontextArg;

    /* Common current pointer and null bit-map */
    int32 nth;		    /* Current count of datum in the block, including NULLs. */
	int32 physical_datum_count;
    bool  has_null;		/* if we have any NULLs at all */

	DatumStreamBitMapWrite	null_bitmap;

    uint8* datump;		/* pointer to datum */

	int32	always_null_bitmap_count;

	/* RLE_TYPE variables */
	bool 	rle_has_compression;

	uint8	*rle_last_item;
	int32	rle_last_item_size;
	bool	rle_last_item_is_repeated;

	int32	rle_total_repeat_items_written;

	DatumStreamBitMapWrite	rle_compress_bitmap;

	int32 	rle_repeatcounts_count;
	int32 	rle_repeatcounts_current_size;

    /* Common buffers */
    MemoryContext memctxt;

    uint8 *null_bitmap_buffer;
    int  null_bitmap_buffer_size;

    uint8 *datum_buffer;
    int32  datum_buffer_size;
	uint8 *datum_afterp;

    /* RLE_TYPE buffers */
    uint8 *rle_compress_bitmap_buffer;
    int32  rle_compress_bitmap_buffer_size;

    int32 *rle_repeatcounts;
    int32  rle_repeatcounts_maxcount;

    /* EOF of current file */
	int64 savings;
	int64 remember_savings;
} DatumStreamBlockWrite;

#define DatumStreamBlockRead_Eyecatcher "DBE"
#define DatumStreamBlockRead_EyecatcherLen 4

typedef struct DatumStreamBlockRead
{
	char		eyecatcher[DatumStreamBlockRead_EyecatcherLen];	
								/* 
								 * Used to validate this is a DatumStreamBlockRead
								 * reference.  3 characters plus NUL.
								 */

	MemoryContext	memctxt;

	DatumStreamTypeInfo	*typeInfo;

	/*
	 * Version of datum stream  block format -- original or RLE_TYPE,
	 */
	DatumStreamVersion	datumStreamVersion;

	bool	rle_can_have_compression;

	int32	maxDataBlockSize;

	int		(*errdetailCallback) (void *errdetailArg);
	void	*errdetailArg;
	int		(*errcontextCallback) (void *errcontextArg);
	void	*errcontextArg;

	/*
	 * Pointer to buffer containing the read data.
	 */
	uint8	*buffer_beginp;

	/*
	 * Information unpacked from the block header, etc.
	 */
	int32	physical_data_size;

	uint8	*null_bitmap_beginp;
	uint8	*datum_beginp;
	uint8	*datum_afterp;
    uint8	*rle_compress_beginp;
	uint8	*rle_repeatcountsp;

	int32 	rle_norepeats_null_bitmap_count;
	int32 	rle_compress_bitmap_count;
	int32 	rle_repeatcounts_count;
	int32 	rle_repeatcounts_size;

    /* Common current pointer and null bit-map */
    int32 	nth;		    /* CURRENT position of datum in the block, including NULLs. */
	int32 	logical_row_count;		
							/* 
							 * Total number of datum in block, including NULLs.
							 */

	int32	physical_datum_index;
	int32	physical_datum_count;

    bool  	has_null;		/* if we have any NULLs at all */

	DatumStreamBitMapRead	null_bitmap;

    uint8* datump;		/* pointer to datum */

	/* Dense write variables */
	bool 	rle_block_was_compressed;

	DatumStreamBitMapRead	rle_compress_bitmap;

	int32	rle_repeatcounts_index;
	bool	rle_in_repeated_item;
	int32	rle_repeated_item_count;

	int32	rle_total_repeat_items_read;
} DatumStreamBlockRead;

extern char *DatumStreamVersion_String(DatumStreamVersion datumStreamVersion);

/*
 * varlena header info to string.
 */
extern char *VarlenaInfoToString(uint8 *p);
extern char *VarlenaInfoToString2(uint8 *p);

/* Stream access method */
extern int DatumStreamBlockRead_Get(DatumStreamBlockRead *dsr, Datum *datum, bool *null);
extern int DatumStreamBlockRead_Advance(DatumStreamBlockRead *dsr);
extern int DatumStreamBlockRead_Nth(DatumStreamBlockRead *dsr);

extern void DatumStreamBlockRead_GetReady(
	DatumStreamBlockRead	*dsr,
	uint8					*buffer,
	int32					bufferSize,
	int64					firstRowNum,
	int32					rowCount,
	bool					*hadToAdjustRowCount,
	int32					*adjustedRowCount);
extern void DatumStreamBlockRead_Reset(DatumStreamBlockRead *dsr);

extern void DatumStreamBlockRead_Init(
	DatumStreamBlockRead 	*dsr,
	DatumStreamTypeInfo 	*typeInfo,
	DatumStreamVersion		datumStreamVersion,
	bool 					rle_can_have_compression,
	int						(*errdetailCallback) (void *errdetailArg),
	void	   				*errdetailArg,
	int						(*errcontextCallback) (void *errcontextArg),
	void	   				*errcontextArg);
extern void DatumStreamBlockRead_Finish(
	DatumStreamBlockRead 	*dsr);

extern void DatumStreamBlockWrite_Init(
	DatumStreamBlockWrite 	*dsw,
	DatumStreamTypeInfo 	*typeInfo,
	DatumStreamVersion		datumStreamVersion,
	bool 					rle_want_compression,
	int32					initialMaxDatumPerBlock,
	int32					maxDatumPerBlock,
	int32					maxDataBlockSize,
	int						(*errdetailCallback) (void *errdetailArg),
	void	   				*errdetailArg,
	int						(*errcontextCallback) (void *errcontextArg),
	void	   				*errcontextArg);
extern void DatumStreamBlockWrite_Finish(
	DatumStreamBlockWrite 	*dsw);

extern int DatumStreamBlockWrite_Put(
	DatumStreamBlockWrite 	*dsw, 
	Datum 					d, 
	bool 					null,
	void					**toFree);
extern int DatumStreamBlockWrite_Nth(DatumStreamBlockWrite *dsw);
extern void DatumStreamBlockWrite_GetReady(
	DatumStreamBlockWrite *dsw);
extern int64 DatumStreamBlockWrite_Block(
	DatumStreamBlockWrite	*dsw,
	uint8 					*buffer);

#endif /* DATUMSTREAMBLOCK_H */

