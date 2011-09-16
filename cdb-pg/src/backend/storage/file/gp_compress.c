/*
 * gp_compress.c
 *      Common compression utilities
 *
 * Copyright (c) 2009, Greenplum Inc.
 * Copyright (c) EMC, 2011
 */

#include "postgres.h"

#include "catalog/pg_compression.h"
#include "cdb/cdbappendonlystoragelayer.h"
#include "storage/gp_compress.h"
#include "utils/guc.h"

static void gp_trycompress_generic(uint8 *sourceData, int32 sourceLen,
								   uint8 *compressedBuffer,
								   int32 compressedBufferLen,
								   int32 *compressedLen,
								   PGFunction compressor,
								   CompressionState *compressionState);

static void gp_decompress_generic(uint8 *compressed, int32 compressedLen,
								  uint8 *uncompressed, int32 uncompressedLen,
								  PGFunction decompressor,
								  CompressionState *compressionState,
								  int64 bufferCount);

CompressorType
CompressorStrToType(const char* comp_str, int comp_level)
{
	CompressorType comptype = COMPTYPE_NONE;

	if (!comp_str || pg_strcasecmp(comp_str, "none") == 0)
	{
		comptype = COMPTYPE_NONE;
	}
	else if (pg_strcasecmp(comp_str, "rle_type") == 0)
	{
		comptype = COMPTYPE_RLE_TYPE;
	}
	else if (pg_strcasecmp(comp_str, "zlib") == 0)
	{
		comptype = COMPTYPE_ZLIB;
	}
	else if (pg_strcasecmp(comp_str, "quicklz") == 0)
	{
		switch(comp_level)
		{
			case 1:
				comptype = COMPTYPE_QUICKLZ1;
				break;
			case 3:
				comptype = COMPTYPE_QUICKLZ3;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERNAL_ERROR),
						 errmsg("Found quicklz with unsupported compression "
								"level %d", comp_level)));
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Found undefined compressor %s", comp_str)));
	}

	return comptype;
}

char *
CompressorTypeToString(CompressorType type)
{
	switch (type)
	{
		case COMPTYPE_ZLIB: return pstrdup("zlib");
		case COMPTYPE_QUICKLZ1: return pstrdup("quicklz");
		case COMPTYPE_QUICKLZ3: return pstrdup("quicklz");
		case COMPTYPE_RLE_TYPE: return pstrdup("rle_type");
		case COMPTYPE_NONE: return pstrdup("none");
	}
	return NULL;
}

int 
gp_trycompress(CompressorType cmpType, int cmplv,
                char *src, int32 srcLen,
                char *comp, int32 *compLen,
                qlz1_state_compress *qlz1StateCompress,
                qlz3_state_compress *qlz3StateCompress
                )
{
        Assert(cmplv > 0);

        /* Zlib */
        if(cmpType == COMPTYPE_ZLIB)
        {
                int err;
                unsigned long len = *compLen;
#ifndef HAVE_LIBZ
                elog(ERROR, "Do not have ZLIB on the system!");
#endif
                err = compress2((unsigned char *) comp, &len, (Bytef *)src, srcLen, cmplv);
                *compLen = len;
                return err;
        }

        /* Quicklz 1 and 3.
         * Unlike zlib, quicklz does not return error if there was not enough
         * room in output buffer.  We will need to convert to an explicit
         * failure so caller can handle it (Z_BUF_ERROR)
         */
        Assert(cmpType == COMPTYPE_QUICKLZ1 || cmpType == COMPTYPE_QUICKLZ3);
        AssertEquivalent(cmpType == COMPTYPE_QUICKLZ1, cmplv == 1);
        AssertEquivalent(cmpType == COMPTYPE_QUICKLZ3, cmplv == 3);

        /* Better have enough buffer */
        Assert(*compLen >= srcLen + MAX_OVERRUN_QUICKLZ);

        if(cmpType == COMPTYPE_QUICKLZ1)
                *compLen = qlz1_compress(src, comp, srcLen, qlz1StateCompress);
        else
                *compLen = qlz3_compress(src, comp, srcLen, qlz3StateCompress);

        /* Assert QuickLz cannot overrun my buffer */
        Assert(*compLen <= srcLen + MAX_OVERRUN_QUICKLZ);

        if(*compLen > srcLen)
                return Z_BUF_ERROR;

        return Z_OK;
}

void
gp_decompress(CompressorType cmpType,
                char *comp, int32 compLen,
                char *decomp, int32 decompLen,
                qlz1_state_decompress *qlz1StateDecompress,
                qlz3_state_decompress *qlz3StateDecompress
                )
{
        if(cmpType == COMPTYPE_ZLIB)
        {
                int err;
                char *errmsg;
                unsigned long len = decompLen;
#ifndef HAVE_LIBZ
                elog(ERROR, "Do not have ZLIB on the system!");
#endif

                err = uncompress((unsigned char *) decomp, &len, (unsigned char *) comp, compLen);
                if(err != Z_OK)
                {
                        switch(err)
                        {
                                case Z_MEM_ERROR:
                                        errmsg = "Insufficient memory";
                                        break;
                                case Z_BUF_ERROR:
                                        errmsg = "Buffer not large enough to decompress";
                                        break;
                                case Z_DATA_ERROR:
                                        errmsg = "Compressed data was corrupted";
                                        break;
                                default:
                                        errmsg = "unknown error";
                                        break;
                        }

                        elog(ERROR, "ZLIB uncompress failed, reason %d, %s", err, errmsg);
                }

                return;
        }

        Assert(cmpType == COMPTYPE_QUICKLZ1 || cmpType == COMPTYPE_QUICKLZ3);

        if(cmpType == COMPTYPE_QUICKLZ1)
        {
                int32 tmpLen = qlz1_size_decompressed(comp);
                if(tmpLen != decompLen)
                        elog(ERROR, "QuickLZ1 decompress error, size does not match");
                tmpLen = qlz1_decompress(comp, decomp, qlz1StateDecompress);
                Assert(tmpLen == decompLen);
        }
        else
        {
                int32 tmpLen = qlz3_size_decompressed(comp);
                if(tmpLen != decompLen)
                        elog(ERROR, "QuickLZ3 decompress error, size does not match");
                tmpLen = qlz3_decompress(comp, decomp, qlz3StateDecompress);
                Assert(tmpLen == decompLen);
        }
}

/*
static void
verifyQlzScratchGuard(
	char		*title,
	char		*qlzScratch,
	int32		qlzScratchLen,
	int32		qlzScratchGuardLen)
{
	int i;

	if (qlzScratchGuardLen == 0)
		return;

	for (i = 0; i < qlzScratchGuardLen; i++)
		if (qlzScratch[qlzScratchLen + i] != 0)
			elog(ERROR,"Guard area after %d byte scratch buffer for '%s' is not zero at index %d",
			     qlzScratchLen, title, i);
}*/


int gp_trycompress_new(
	 uint8		*sourceData,
	 int32		sourceLen,
	 uint8		*compressedBuffer,
	 int32		compressedBufferWithOverrrunLen,
	 int32		maxCompressedLen,	// The largest compressed result we would like to see.
	 int32		*compressedLen,
	 int		compressLevel,
	 CompressorType  compressType,
	 PGFunction     compressor,
	 CompressionState *compressionState)
{
	Insist(PointerIsValid(compressor));

	gp_trycompress_generic(sourceData, sourceLen, compressedBuffer,
						   compressedBufferWithOverrrunLen, compressedLen,
						   compressor, compressionState);

	/* XXX: this interface is AWFUL! */
	return Z_OK;
}

/*---------------------------------------------------------------------------*/
static void
gp_trycompress_generic( uint8			*sourceData
						  , int32			 sourceLen
						  , uint8			*compressedBuffer
						  , int32			 compressedBufferLen
						  , int32			*compressedLen
						  , PGFunction		compressor
						  , CompressionState *compressionState
						  )

{
	callCompressionActuator( compressor
						   , (const void *)sourceData
						   , (size_t)sourceLen
						   , (char*)compressedBuffer
						   , compressedBufferLen
						   , compressedLen
						   , compressionState
						   );
}

/*---------------------------------------------------------------------------*/
static void gp_decompress_generic(
	  uint8	 *compressed,
	  int32	 compressedLen,
	  uint8	 *uncompressed,
	  int32	 uncompressedLen,
	  PGFunction	 decompressor,
	  CompressionState *compressionState,
	  int64	 bufferCount
	  )
{
	unsigned long compressedSize;
	unsigned long uncompressedSize;
	int32				 resultingUncompressedLen;

	compressedSize = (unsigned long)compressedLen;
	uncompressedSize = (unsigned long)uncompressedLen;

	callCompressionActuator(decompressor,
							(const void *)compressed,
							(size_t)compressedLen,
							(char*)uncompressed,
							uncompressedLen,
							&resultingUncompressedLen,
							compressionState);

	if (resultingUncompressedLen != uncompressedLen)
		elog(ERROR,
			 "Uncompress returned length %d which is different than the "
			 "expected length %d (block count " INT64_FORMAT ")",
			 resultingUncompressedLen,
			 uncompressedLen,
			 bufferCount);

}  /* end gp_decompress_generic */

/*---------------------------------------------------------------------------*/
void gp_decompress_new(
			uint8			*compressed,
			int32			compressedLen,
			uint8			*uncompressed,
			int32			uncompressedLen,
			CompressorType	compresstype,	/* the compressor type used */
			PGFunction     decompressor,
			CompressionState *compressionState,
			qlz1_state_decompress *qlz1StateDecompress,
			qlz3_state_decompress *qlz3StateDecompress,
			int64			bufferCount)
{
	unsigned long compressedSize;
	unsigned long uncompressedSize;

	int32 resultingUncompressedLen;

	compressedSize = (unsigned long)compressedLen;
	uncompressedSize = (unsigned long)uncompressedLen;

	if (decompressor != NULL)
	{
		gp_decompress_generic( compressed
						, compressedLen
						, uncompressed
						, uncompressedLen
						, decompressor
						, compressionState
						, bufferCount
		);

	    return;
	}

	if(compresstype == COMPTYPE_ZLIB)
	{
		int res;

#ifndef HAVE_LIBZ
		ereport(ERROR,
				(errcode(ERRCODE_GP_INTERNAL_ERROR),
				 errmsg("Don't have ZLIB on the system!")));
#else
		res = uncompress(uncompressed, &uncompressedSize,
						 (Bytef *)compressed, compressedSize);

		if (res != Z_OK)
		{
			char *detail = NULL;

			switch(res)
			{
				case Z_MEM_ERROR:
					detail = "Insufficient memory";
					break;
				case Z_BUF_ERROR:
					detail = "The buffer was not large enough to hold the uncompressed data";
					break;
				case Z_DATA_ERROR:
					detail = "The compressed data was corrupted";
					break;
				default:
					elog(ERROR,"ZLIB uncompress failed -- reason %d unknown (output buffer length %d, compressed length %d)",
						 res, uncompressedLen, compressedLen);
					break;
			}

			elog(ERROR,"ZLIB uncompress failed (detail: '%s', output buffer length %d, compressed length %d, (block count " INT64_FORMAT "))",
				 detail, uncompressedLen, compressedLen, bufferCount);
		}
#endif
	}
	else if(compresstype == COMPTYPE_QUICKLZ1)
	{
		//if (Debug_appendonly_guard_end_quicklz_scratch)
		//	verifyQlzScratchGuard("qlz1_decompress (check before)", qlzScratch, qlzScratchLen, qlzScratchGuardLen);
		//if (Debug_appendonly_rezero_quicklz_decompress_scratch)
		//	memset(qlzScratch, 0, qlzScratchLen);

		uncompressedSize = qlz1_decompress((const char *)compressed,
										  uncompressed,
										  qlz1StateDecompress);

		//if (Debug_appendonly_guard_end_quicklz_scratch)
		//	verifyQlzScratchGuard("qlz1_decompress (check after)", qlzScratch, qlzScratchLen, qlzScratchGuardLen);

	}
	else
	{
		Insist(compresstype == COMPTYPE_QUICKLZ3);

		//if (Debug_appendonly_guard_end_quicklz_scratch)
		//	verifyQlzScratchGuard("qlz3_decompress (check before)", qlzScratch, qlzScratchLen, qlzScratchGuardLen);
		//if (Debug_appendonly_rezero_quicklz_decompress_scratch)
		//	memset(qlzScratch, 0, qlzScratchLen);

		uncompressedSize = qlz3_decompress((const char *)compressed,
										  uncompressed,
										  qlz3StateDecompress);

		//if (Debug_appendonly_guard_end_quicklz_scratch)
		//	verifyQlzScratchGuard("qlz3_decompress (check after)", qlzScratch, qlzScratchLen, qlzScratchGuardLen);

	}

	resultingUncompressedLen = (int32)uncompressedSize;

	if (resultingUncompressedLen != uncompressedLen)
		elog(ERROR,
			 "Uncompress returned length %d which is different than the "
			 "expected length %d (block count " INT64_FORMAT ")",
			 resultingUncompressedLen,
			 uncompressedLen,
			 bufferCount);
}

void
gp_issuecompresserror(
	int				zlibCompressError,
	int32			sourceLen)
{
	char *detail = NULL;

	switch(zlibCompressError)
	{
	case Z_MEM_ERROR:
		detail = "Insufficient memory";
		break;
	case Z_BUF_ERROR:
		detail = "The buffer was not large enough to hold the compressed data";
		break;
	case Z_STREAM_ERROR:
		detail = "The level was not Z_DEFAULT_LEVEL, or was not between 0 and 9";
		break;
	default:
		elog(ERROR,"ZLIB compress2 failed -- reason %d unknown (uncompressed length %d)",
			 zlibCompressError, sourceLen);
		break;
	}

	elog(ERROR,"ZLIB compress2 failed (detail: '%s', uncompressed length %d)",
		 detail, sourceLen);
}


