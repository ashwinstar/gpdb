/*-------------------------------------------------------------------------
 *
 * appendonlystorage.h
 *
 * Copyright (c) 2007-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDONLYSTORAGE_H
#define APPENDONLYSTORAGE_H

#include "c.h"
#include "utils/pg_crc.h"


#define AO_HEADER_LEN 8	// 64 bits.

#define AOBlockHeader_MaxRowCount 0x3FFF   // 14 bits, or 16,383 (16k  1).
#define AOBlockHeader_MaxLength   0x1FFFFF // 21 bits, or 2,097,151 (2 Mb  1).

#define AOStorage_RoundUp(l) ((((l)+3)/4)*4)

#define	AOStorage_ZeroPad(\
			buf,\
			len,\
			padToLen)\
{\
	int b;\
\
	for (b = len; b < padToLen; b++)\
		buf[b] = 0;\
}

typedef enum AoHeaderKind
{
	AoHeaderKind_None = 0,
	AoHeaderKind_Block,
	MaxAoHeaderKind /* must always be last */
} AoHeaderKind;


typedef enum AOHeaderCheckError
{
	AOHeaderCheckOk = 0,
	AOHeaderCheckFirst32BitsAllZeroes,
	AOHeaderCheckReservedBit0Not0,
	AOHeaderCheckInvalidHeaderKindNone,
	AOHeaderCheckInvalidHeaderKind,
	AOHeaderCheckRowCountIsZero,
	AOHeaderCheckInvalidCompressedLen,
	AOHeaderCheckInvalidOverallBlockLen,
} AOHeaderCheckError;


/* 
 * AOTupleId is a unique tuple id, specific to AO 
 * relation tuples, of the form (segfile#, row#)
 * 
 * Out of the following 48 bits, the 38 leftmost
 * bits stand for the row within the segment file 
 * (Limit: 274 trillion (2^38) - 1), and the 10
 * rightmost bits stand for which segment file the 
 * tuple is in (Limit: 1023 (2^10 - 1))
 */
typedef struct AOTupleId
{
	uint32		bytes_0_3;
	uint16		bytes_4_5;

} AOTupleId;

   
#define AOTupleIdGet_segmentFileNum(h)        (((h)->bytes_4_5&0x03FF)) // 10 bits
#define AOTupleIdGet_rowNum(h) \
          ((((uint64)((h)->bytes_0_3))<<6)|(((uint64)((h)->bytes_4_5&0xFC00))>>10))
         /* top most 32 bits (all bytes_2_3) */  /* 6 top bits from bytes_4_5 */

/* For single bits, set or clear directly.  Otherwise, use AND to clear field then OR to set.
#define AOTupleIdSet_segmentFileNum(h,e)         {(h)->bytes_0_1&=0x03FF;(h)->bytes_0_1|=((0x03FF&((uint16)(e)))<<6);}
#define AOTupleIdSet_rowNum(h,e)    {(h)->bytes_0_1&=0xFFC0;(h)->bytes_0_1|=(uint16)((0x0000003F00000000&(e))>>32);\
                                     (h)->bytes_2_5=(uint32)(e);}
*/
// Assume field is initially zero.
#define AOTupleIdInit_Init(h)                {(h)->bytes_0_3=0;(h)->bytes_4_5=0;}
#define AOTupleIdInit_segmentFileNum(h,e)    {(h)->bytes_4_5|=(0x03FF&((uint16)(e)));}
#define AOTupleIdInit_rowNum(h,e)            {(h)->bytes_0_3|=((uint32)((INT64CONST(0x0000003FFFFFFFFF)&(e))>>6));\
											  (h)->bytes_4_5|=((0x003F&((uint16)(e)))<<10);}

#define AOTupleId_MaxRowNum            INT64CONST(274877906943) 		// 38 bits, or 274,877,906,943 (274 trillion  1).
#define AOTupleId_MaxRowNum_CommaStr  "274,877,906,943"


extern int32 AppendOnlyStorage_HeaderLenNeeded(
	bool			usingChecksum);

extern void AppendOnlyStorage_MakeBlockHeader(
	uint8			*headerPtr,
	bool			usingChecksums,
	int				executorKind,
	int				rowCount,
	int32			dataLength,
	int32			compressedLength);

extern void AppendOnlyStorage_LogBlockHeader(
	char			*tableName,
	int				segmentFileNum,
	int64			headerOffsetInFile,
	uint8			*headerPtr,
	bool			usingChecksums);

extern int errdetail_appendonly_block_header(
	uint8	*header,
	bool	usingChecksum);

extern char *AppendOnlyStorage_GetHeaderCheckErrorStr(void);

extern AOHeaderCheckError AppendOnlyStorage_GetHeaderInfo(
	uint8			*headerPtr,
	int				headerLen,
	AoHeaderKind	*headerKind);

extern AOHeaderCheckError AppendOnlyStorage_GetBlockHeaderInfo(
	uint8			*headerPtr,
	int				headerLen,
	bool			usingChecksums,
	int32			blockLimitLen,
	int32			*overallBlockLen,
	int32			*offset,
	int32			*length,
	int				*executorBlockKind,
	int				*rowCount,
	bool			*isCompressed);

extern bool AppendOnlyStorage_VerifyHeaderChecksum(
	uint8			*headerPtr,
	pg_crc32		*storedChecksum,
	pg_crc32		*computedChecksum);

extern bool AppendOnlyStorage_VerifyBlockChecksum(
	uint8			*headerPtr,
	int32			overallBlockLen,
	pg_crc32		*storedChecksum,
	pg_crc32		*computedChecksum);

extern int32 AppendOnlyStorage_GetUncompressedLen(
	uint8			*headerPtr);

extern char* AOTupleIdToString(AOTupleId * aoTupleId);

#endif   /* APPENDONLYSTORAGE_H */
