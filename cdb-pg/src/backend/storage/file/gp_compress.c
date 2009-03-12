/*
 * gp_compress.c
 *      Common compression utilities
 *    
 * Copyright (c) 2009, Greenplum INc.
 */

#include "postgres.h"
#include "storage/gp_compress.h"

CompressorType CompressorStrToType(const char* comp_str, int comp_level)
{
	CompressorType comptype = COMPTYPE_NONE;
	
	if (!comp_str)
	{
		comptype = COMPTYPE_NONE;		
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

int gp_trycompress(CompressorType cmpType, int cmplv,
                char *src, int32 srcLen,
                char *comp, int32 *compLen,
                char *scratch
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
                *compLen = qlz1_compress(src, comp, srcLen, scratch);
        else
                *compLen = qlz3_compress(src, comp, srcLen, scratch);

        /* Assert QuickLz cannot overrun my buffer */
        Assert(*compLen <= srcLen + MAX_OVERRUN_QUICKLZ);

        if(*compLen > srcLen)
                return Z_BUF_ERROR;

        return Z_OK;
}

void gp_decompress(CompressorType cmpType,
                char *comp, int32 compLen,
                char *decomp, int32 decompLen,
                char *scratch
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
                tmpLen = qlz1_decompress(comp, decomp, scratch);
                Assert(tmpLen == decompLen);
        }
        else
        {
                int32 tmpLen = qlz3_size_decompressed(comp);
                if(tmpLen != decompLen)
                        elog(ERROR, "QuickLZ3 decompress error, size does not match");
                tmpLen = qlz3_decompress(comp, decomp, scratch);
                Assert(tmpLen == decompLen);
        }
}

