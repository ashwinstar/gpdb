/*
 * gp_compress.h
 *      gpdb compression utilities.
 *
 * Copyright (c) 2009, Greenplum Inc. 
 */

#ifndef GP_COMPRESS_H
#define GP_COMPRESS_H

#include "storage/quicklz1.h"
#include "storage/quicklz3.h"

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

/*
 *	Represents the compressor type used for an AO table.
 *	Note that we treat quicklz with compression levels 1 and 3 as
 *	two separate types of compressors, this is because we have to
 *	compile them separately and have a separate API for each. Zlib
 *	compression will have a compression level attached to it, checked
 *	separately. 
 */
typedef enum CompressorType
{
	COMPTYPE_NONE,		/* no compression used */
	COMPTYPE_ZLIB,	
	COMPTYPE_QUICKLZ1,
	COMPTYPE_QUICKLZ3
} CompressorType;

// UNDONE: Is there a QLZ DEFINE for this?
#define MAX_OVERRUN_QUICKLZ 400

extern CompressorType CompressorStrToType(const char* comp_str, int comp_level);

/* Decompress always works, or we will throw.  So no need for return value */
extern void gp_decompress(CompressorType cmptype, 
                char *comp,
                int32 compLen,
                char *decomp,
                int32 decompLen,
                qlz1_state_decompress *qlz1StateDecompress,
                qlz3_state_decompress *qlz3StateDecompress
                );

/* Try to compress.  If comp buff too small, we will return error */
extern int gp_trycompress(CompressorType cmptype, int cmplv,
                char *src,
                int32 srcLen,
                char *comp,
                int32 *compLen,
                qlz1_state_compress *qlz1StateCompress,
                qlz3_state_compress *qlz3StateCompress
                );

extern int gp_trycompress_new(
		 uint8		*sourceData,
		 int32		sourceLen,
		 uint8		*compressedBuffer,
		 int32		compressedBufferWithOverrrunLen,
		 int32		maxCompressedLen,	// The largest compressed result we would like to see.
		 int32		*compressedLen,
		 int		compressLevel,
		 CompressorType  compressType,
		 qlz1_state_compress *qlz1StateCompress,
		 qlz3_state_compress *qlz3StateCompress);

extern void gp_decompress_new(
				uint8			*compressed,
				int32			compressedLen,
				uint8			*uncompressed,
				int32			uncompressedLen,
				CompressorType	compresstype,	/* the compressor type used */
				qlz1_state_decompress *qlz1StateDecompress,
				qlz3_state_decompress *qlz3StateDecompress,
				int64			bufferCount);

extern void gp_issuecompresserror(
	int				zlibCompressError,
	int32			sourceLen);

#endif
