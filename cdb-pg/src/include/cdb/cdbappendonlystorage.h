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

#define AOStorage_RoundUp4(l) ((((l)+3)/4)*4) /* 32-bit alignment */

#define AOStorage_RoundUp8(l) ((((l)+7)/8)*8) /* 64-bit alignment */

#define AOStorage_RoundUp(l, version) \
	((IsAOBlockAndMemtupleAlignmentFixed(version)) ? (AOStorage_RoundUp8(l)) : (AOStorage_RoundUp4(l)))

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

extern int32 AppendOnlyStorage_GetUsableBlockSize(
	int32 configBlockSize);

#endif   /* CDBAPPENDONLYSTORAGE_H */
