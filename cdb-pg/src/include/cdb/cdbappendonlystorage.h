/*-------------------------------------------------------------------------
 *
 * cdbappendonlystorage.h
 *
 * Copyright (c) 2007-2009, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBAPPENDONLYSTORAGE_H
#define CDBAPPENDONLYSTORAGE_H

#include "c.h"
#include "utils/pg_crc.h"


#define AO_HEADER_LEN 8	// 64 bits.

#define AOBlockHeader_MaxRowCount 0x3FFF   // 14 bits, or 16,383 (16k-1).
										   // Maximum row count for small content.
										   
#define AOBlockHeader_MaxLength   0x1FFFFF // 21 bits, or 2,097,151 (2 Mb-1).
										   // Maximum blocksize.  Does not include
										   // Append-Only Storage Header overhead...


#define AOLargeContentHeader_MaxLargeRowCount 0x1FFFFFF 
										   // 25 bits, or 33,554,431, or (2^25-1)
										   // Maximum row count for large content.

#define AOLargeContentHeader_MaxLargeContentLength   0x3FFFFFFF 
										  // 30 bits, or 1,073,741,823 (1Gb-1).

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
	AoHeaderKind_SmallContent = 1,
	AoHeaderKind_LargeContent = 2,
	MaxAoHeaderKind /* must always be last */
} AoHeaderKind;


typedef enum AOHeaderCheckError
{
	AOHeaderCheckOk = 0,
	AOHeaderCheckFirst32BitsAllZeroes,
	AOHeaderCheckReservedBit0Not0,
	AOHeaderCheckInvalidHeaderKindNone,
	AOHeaderCheckInvalidHeaderKind,
	AOHeaderCheck__OBSOLETE_,
	AOHeaderCheckInvalidCompressedLen,
	AOHeaderCheckInvalidOverallBlockLen,
	AOHeaderCheckLargeContentLenIsZero,
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
 *
 *
 * ***WARNING*** STRUCT PACKING ISSUE.
 *
 *       Previously, we had the uint16 first and the uint32 second.  But on some compliers
 *       the second uint32 would get packed on a 32-bit boundary and the struct would end
 *       up with a hole between the fields...  Arranging the uint16 second is known to work
 *       correctly since the ItemPointerData has the same layout.  And, apparently putting
 *       small fields after big ones is a policy in system catalog struct layouts.
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

#define AOTupleId_MaxRowNum            INT64CONST(274877906943) 		// 38 bits, or 274,877,906,943 (274 million  - 1).
#define AOTupleId_MaxRowNum_CommaStr  "274,877,906,943"

extern char* AOTupleIdToString(AOTupleId * aoTupleId);

extern int32 AppendOnlyStorage_GetUsableBlockSize(
	int32 configBlockSize);

#endif   /* CDBAPPENDONLYSTORAGE_H */
