/*-------------------------------------------------------------------------
 *
 * cdbappendonlystorageformat.c
 *
 * Copyright (c) 2007-2009, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "storage/gp_compress.h"
#include "cdb/cdbappendonlystorage_int.h"
#include "cdb/cdbappendonlystorage.h"
#include "cdb/cdbappendonlystorageformat.h"
#include "utils/pg_crc.h"
#include "utils/guc.h"

static pg_crc32
AppendOnlyStorageFormat_ComputeHeaderChecksum(
	uint8			*headerPtr,
	int32			headerLen)
{
	pg_crc32	crc;
	
	Assert(headerPtr != NULL);

	INIT_CRC32(crc);

	/* 
	 * Compute CRC of the header. The header length does not include the
	 * header checksum.
	 */
	COMP_CRC32(crc, 
	           headerPtr, 
	           headerLen);

	/* BUG: This routine does not return a correct CRC32, since it never calls FIN_CRC32 */
	return crc;
}

static pg_crc32
AppendOnlyStorageFormat_ComputeBlockChecksum(
	uint8			*headerPtr,
	int32			headerLen,
	int32			overallBlockLen)
{
	int32		dataOffset;
	pg_crc32	crc;
	
	Assert(headerPtr != NULL);

	/*
	 * The block checksum covers right after the header checksum through
	 * the end of the whole block (including the optional firstRowNum).
	 */
	dataOffset = headerLen + sizeof(pg_crc32);

	INIT_CRC32(crc);

	/* Compute CRC of the header. */
	COMP_CRC32(crc, 
	           headerPtr + dataOffset, 
	           overallBlockLen - dataOffset);

	/* BUG: This routine does not return a correct CRC32, since it never calls FIN_CRC32 */
	return crc;
}


int32 
AppendOnlyStorageFormat_HeaderLenNeeded(
	bool			usingChecksum)
{
	return AO_HEADER_LEN +
		   (usingChecksum ? 2 * sizeof(pg_crc32) : 0);
}

static void
AppendOnlyStorageFormat_AddFirstRowNum(
	uint8			*headerPtr,
	bool			usingChecksums,
	int64			firstRowNum)
{
	AOBlockHeader 	*blockHeader = (AOBlockHeader*)headerPtr;
	
	int32		offsetToFirstRowNum;
	int64		*firstRowNumPtr;

	offsetToFirstRowNum = AO_HEADER_LEN;
	if (usingChecksums) 
		offsetToFirstRowNum += 2 * sizeof(pg_crc32); // Header and Block checksums.
				
	firstRowNumPtr = (int64*)&headerPtr[offsetToFirstRowNum];
	*firstRowNumPtr = firstRowNum;
	
	if (Debug_appendonly_print_storage_headers)
		elog(LOG,
			 "Append-Only storage first row number header result: block_bytes_0_3 0x%X, block_bytes_4_7 0x%X, "
			 "firstRowNum " INT64_FORMAT,
			 blockHeader->block_bytes_0_3,
			 blockHeader->block_bytes_4_7,
			 firstRowNum);
}

static void
AppendOnlyStorageFormat_AddBlockHeaderChecksums(
	uint8			*headerPtr,
	bool			isCompressed,
	bool			hasFirstRowNum,
	int				version,
	int32			dataLength,
	int32			compressedLength)
{
	AOBlockHeader 	*blockHeader = (AOBlockHeader*)headerPtr;
	
	int32		headerLen;
	int32		overallBlockLen;
	pg_crc32	*blockChecksumPtr;
	pg_crc32	*headerChecksumPtr;

	headerLen = AO_HEADER_LEN + 
				sizeof(pg_crc32);			// Block checksum.
				
	overallBlockLen = headerLen +
					  sizeof(pg_crc32);		// Header checksum.
	if (hasFirstRowNum)
		overallBlockLen += sizeof(int64);
	overallBlockLen += AOStorage_RoundUp(
	                        (isCompressed ? compressedLength : dataLength),
	                        version);
	
	/*
	 * Calculate block checksum first since it is included in the 
	 * header checksum.
	 */
	blockChecksumPtr = (pg_crc32*)&headerPtr[AO_HEADER_LEN];
	*blockChecksumPtr = AppendOnlyStorageFormat_ComputeBlockChecksum(
													headerPtr,
													headerLen, 
													overallBlockLen);
	/*
	 * Now the Header checksum after the whole header.
	 */
	headerChecksumPtr = (pg_crc32*)&headerPtr[headerLen];
	*headerChecksumPtr = AppendOnlyStorageFormat_ComputeHeaderChecksum(
															headerPtr,
															headerLen);
	
	if (Debug_appendonly_print_storage_headers)
		elog(LOG,
			 "Append-Only storage make block header result: block_bytes_0_3 0x%X, block_bytes_4_7 0x%X, "
			 "header checksum 0x%X, block checksum 0x%X, overallBlockLen %d",
			 blockHeader->block_bytes_0_3,
			 blockHeader->block_bytes_4_7,
			 *headerChecksumPtr,
			 *blockChecksumPtr,
			 overallBlockLen);
}

void
AppendOnlyStorageFormat_MakeSmallContentHeader(
	uint8			*headerPtr,
	bool			usingChecksums,
	bool			hasFirstRowNum,
	int				version,
	int64			firstRowNum,
	int				executorKind,
	int				rowCount,
	int32			dataLength,
	int32			compressedLength)
{
	AOBlockHeader 	*blockHeader;
	bool			isCompressed;

	Assert(headerPtr != NULL);
	
	blockHeader = (AOBlockHeader*)headerPtr;

	if (Debug_appendonly_print_storage_headers)
		elog(LOG,
			 "Append-Only storage make Small Content header parameters: wantChecksum = %s, hasFirstRowNum %s, executorKind = %d, "
			 "rowCount = %d, dataLength %d, compressedLength %d",
			 (usingChecksums ? "true" : "false"),
			 (hasFirstRowNum ? "true" : "false"),
			 executorKind,
			 rowCount,
			 dataLength,
			 compressedLength);
	
	/* Zero out whole header */
	AOBlockHeaderInit_Init(blockHeader);
	
	AOBlockHeaderInit_headerKind(blockHeader,AoHeaderKind_SmallContent);
	AOBlockHeaderInit_executorBlockKind(blockHeader,executorKind);
	AOBlockHeaderInit_rowCount(blockHeader,rowCount);
	AOBlockHeaderInit_dataLength(blockHeader,dataLength);
	AOBlockHeaderInit_hasFirstRowNum(blockHeader,hasFirstRowNum);
	
	isCompressed = (compressedLength > 0);
	if (isCompressed)
		AOBlockHeaderInit_compressedLength(blockHeader,compressedLength);

	/*
	 * Add the optional firstRowNum.
	 *
	 * NOTE: This is not part of the 8-byte (64-bit) header because it is so big.
	 * NOTE: And, it is not covered by the header checksum because in order to
	 * NOTE: determine if we should checksum more data we would need to examine
	 * NOTE: the header data not verified by checksum yet...
	 *
	 * So, the firstRowNum is extra data between the header (and checksums) and
	 * the content.  We must add it before computing the checksum.
	 */
	if (hasFirstRowNum)
	{
		AppendOnlyStorageFormat_AddFirstRowNum(
											headerPtr,
											usingChecksums,
											firstRowNum);
	}
	
	if (usingChecksums)
	{
		AppendOnlyStorageFormat_AddBlockHeaderChecksums(
											headerPtr,
											isCompressed,
											hasFirstRowNum,
											version,
											dataLength,
											compressedLength);
	}
	else
	{
		if (Debug_appendonly_print_storage_headers)
			elog(LOG,
				 "Append-Only storage make block header result: block_bytes_0_3 0x%X, block_bytes_4_7 0x%X",
				 blockHeader->block_bytes_0_3,
				 blockHeader->block_bytes_4_7);
	}

}

void
AppendOnlyStorageFormat_LogBlockHeader(
	char			*tableName,
	char			*segmentFileName,
	int64			headerOffsetInFile,
	uint8			*headerPtr,
	bool			usingChecksums,
	int				version)
{
	AOBlockHeader 	*blockHeader;
	int32			headerLen;
	int32			wholeHeaderLen;

	int			executorBlockKind;
	int			rowCount;
	int32		dataLength;
	int32		compressedLength;
	int32		overallBlockLen;

	pg_crc32	*blockChecksumPtr = NULL;
	pg_crc32	blockChecksum;
	pg_crc32	*headerChecksumPtr = NULL;
	pg_crc32	headerChecksum;

	Assert(headerPtr != NULL);

	blockHeader = (AOBlockHeader*)headerPtr;

	headerLen = AO_HEADER_LEN;

	executorBlockKind  = 	AOBlockHeaderGet_executorBlockKind(blockHeader);
	rowCount = 				AOBlockHeaderGet_rowCount(blockHeader);
	
	wholeHeaderLen =		headerLen + 
							(usingChecksums ? 2 * sizeof(pg_crc32) : 0);
	
	dataLength =			AOBlockHeaderGet_dataLength(blockHeader);
	
	compressedLength =		AOBlockHeaderGet_compressedLength(blockHeader);

	overallBlockLen =		wholeHeaderLen +
	                        AOStorage_RoundUp(
	                        	(compressedLength == 0 ? dataLength : compressedLength),
	                        	version);

	if (usingChecksums)
	{
		blockChecksumPtr = (pg_crc32*)&headerPtr[headerLen];
		blockChecksum = *blockChecksumPtr;
		
		headerLen += sizeof(pg_crc32);
		
		headerChecksumPtr = (pg_crc32*)&headerPtr[headerLen];
		headerChecksum = *headerChecksumPtr;
	}
	else
	{
		blockChecksum = 0;
		headerChecksum = 0;
	}

	elog(LOG,
		 "Append-Only storage long header: table '%s', segment file '%s', header offset in file " INT64_FORMAT ", "
		 "block_bytes_0_3 0x%X, block_bytes_4_7 0x%X, "
		 "executorBlockKind = %d, "
		 "rowCount = %d, usingChecksums = %s, header checksum 0x%X, block checksum 0x%X, "
		 "dataLength %d, compressedLength %d, overallBlockLen %d",
		 tableName,
		 segmentFileName,
		 headerOffsetInFile,
		 blockHeader->block_bytes_0_3,
		 blockHeader->block_bytes_4_7,
		 executorBlockKind,
		 rowCount,
		 (usingChecksums ? "true" : "false"),
		 headerChecksum,
		 blockChecksum,
		 dataLength,
		 compressedLength,
		 overallBlockLen);
}


/*
 * errdetail_appendonly_storage_block_header
 *
 * Add an errdetail() line showing the Append-Only Storage block header.
 */
int
errdetail_appendonly_storage_block_header(
	uint8	*header,
	bool	usingChecksum,
	int		version)
{
	AOBlockHeader 	*blockHeader;
	int32			headerLen;
	int32			wholeHeaderLen;

	int			executorBlockKind;
	int			rowCount;
	int32		uncompressedLength;
	int32		compressedLength;
	int32		overallBlockLen;

	pg_crc32	*blockChecksumPtr = NULL;
	pg_crc32	blockChecksum;
	pg_crc32	*headerChecksumPtr = NULL;
	pg_crc32	headerChecksum;

	blockHeader = (AOBlockHeader*)header;

	headerLen = AO_HEADER_LEN;

	executorBlockKind  = 	AOBlockHeaderGet_executorBlockKind(blockHeader);
	rowCount = 				AOBlockHeaderGet_rowCount(blockHeader);
	
	wholeHeaderLen =		headerLen + 
							(usingChecksum ? 2 * sizeof(pg_crc32) : 0);
	
	uncompressedLength =	AOBlockHeaderGet_dataLength(blockHeader);
	
	compressedLength =		AOBlockHeaderGet_compressedLength(blockHeader);

	overallBlockLen =		wholeHeaderLen +
	                        AOStorage_RoundUp(
	                        	(compressedLength == 0 ? uncompressedLength : compressedLength),
	                        	version);

	if (usingChecksum)
	{
		blockChecksumPtr = (pg_crc32*)&header[headerLen];
		blockChecksum = *blockChecksumPtr;
		
		headerLen += sizeof(pg_crc32);
		
		headerChecksumPtr = (pg_crc32*)&header[headerLen];
		headerChecksum = *headerChecksumPtr;
	}
	else
	{
		blockChecksum = 0;
		headerChecksum = 0;
	}

	errdetail( 
		 "Append-Only storage header: "
		 "block_bytes_0_3 0x%X, block_bytes_4_7 0x%X, "
		 "executorBlockKind = %d, "
		 "rowCount = %d, usingChecksums = %s, header checksum 0x%X, block checksum 0x%X, "
		 "uncompressedLength %d, compressedLength %d, overallBlockLen %d",
		 blockHeader->block_bytes_0_3,
		 blockHeader->block_bytes_4_7,
		 executorBlockKind,
		 rowCount,
		 (usingChecksum ? "true" : "false"),
		 headerChecksum,
		 blockChecksum,
		 uncompressedLength,
		 compressedLength,
		 overallBlockLen);	

	return 0;
}

int 
errdetail_appendonly_storage_smallcontent_header(
	uint8	*header,
	bool	usingChecksum)
{
	// UNDONE
	
	return 0;
}

int 
errdetail_appendonly_storage_largecontent_header(
	uint8	*header,
	bool	usingChecksum)
{
	// UNDONE

	return 0;
}


void
AppendOnlyStorageFormat_MakeLargeContentHeader(
	uint8			*headerPtr,
	bool			usingChecksums,
	bool			hasFirstRowNum,
	int				version,
	int64			firstRowNum,
	int				executorKind,
	int				largeRowCount,
	int32			largeContentLength)
{
	AOLargeContentHeader 	*largeContentHeader;

	Assert(headerPtr != NULL);
	
	largeContentHeader = (AOLargeContentHeader*)headerPtr;

	if (Debug_appendonly_print_storage_headers)
		elog(LOG,
			 "Append-Only Storage make LargeContent header parameters: usingChecksums = %s, executorKind = %d, "
			 "largeRowCount = %d, largeContentLength %d",
			 (usingChecksums ? "true" : "false"),
			 executorKind,
			 largeRowCount,
			 largeContentLength);
	
	/* Zero out whole header */
	AOLargeContentHeaderInit_Init(largeContentHeader);
	
	AOLargeContentHeaderInit_headerKind(largeContentHeader,AoHeaderKind_LargeContent);
	AOLargeContentHeaderInit_executorBlockKind(largeContentHeader,executorKind);
	AOLargeContentHeaderInit_largeRowCount(largeContentHeader,largeRowCount);
	AOLargeContentHeaderInit_largeContentLength(largeContentHeader,largeContentLength);
	AOLargeContentHeaderInit_hasFirstRowNum(largeContentHeader,hasFirstRowNum);

	/*
	 * Add the optional firstRowNum.
	 *
	 * NOTE: This is not part of the 8-byte (64-bit) header because it is so big.
	 * NOTE: And, it is not covered by the header checksum because in order to
	 * NOTE: determine if we should checksum more data we would need to examine
	 * NOTE: the header data not verified by checksum yet...
	 *
	 * So, the firstRowNum is extra data between the header (and checksums) and
	 * the content.  We must add it before computing the checksum.
	 */
	if (hasFirstRowNum)
	{
		AppendOnlyStorageFormat_AddFirstRowNum(
											headerPtr,
											usingChecksums,
											firstRowNum);
	}
	
	if (usingChecksums)
	{
		// UNDONE: Set 2nd checksum to 0 when there is no content???
		AppendOnlyStorageFormat_AddBlockHeaderChecksums(
											headerPtr,
											/* isCompressed */ false,
											/* hasFirstRowNum */ false,
											version,
											/* dataLength */ 0,
											/* compressedLength */ 0);
	}
	else
	{
		if (Debug_appendonly_print_storage_headers)
			elog(LOG,
				 "Append-Only storage make block header result: block_bytes_0_3 0x%X, block_bytes_4_7 0x%X",
				 largeContentHeader->largecontent_bytes_0_3,
				 largeContentHeader->largecontent_bytes_4_7);
	}
}


#define MAX_AOHEADER_CHECK_ERROR_STR 300
static char AoHeaderCheckErrorStr[MAX_AOHEADER_CHECK_ERROR_STR] = "\0";

/*
 * Return a string message for the last check error.
 */
char *AppendOnlyStorageFormat_GetHeaderCheckErrorStr(void)
{
	return AoHeaderCheckErrorStr;
}

AOHeaderCheckError
AppendOnlyStorageFormat_GetHeaderInfo(
	uint8			*headerPtr,
	bool			usingChecksums,
	AoHeaderKind	*headerKind,
	int32			*actualHeaderLen)
{
	AOHeader 	*header;
	int snprintfResult;

	Assert(headerPtr != NULL);
	Assert(headerKind != NULL);

	header = (AOHeader*)headerPtr;

	if (header->header_bytes_0_3 == 0)
	{
		snprintfResult =
			snprintf(
				AoHeaderCheckErrorStr,
				MAX_AOHEADER_CHECK_ERROR_STR,
				"Append-only storage header is invalid -- first 32 bits are all zeroes (header_bytes_0_3 0x%08x, header_bytes_4_7 0x%08x)",
			    header->header_bytes_0_3, header->header_bytes_4_7);
		
		Assert(snprintfResult >= 0);
		Assert(snprintfResult < MAX_AOHEADER_CHECK_ERROR_STR);
		
		return AOHeaderCheckFirst32BitsAllZeroes;
	}

	if (AOHeaderGet_reserved0(header) != 0)
	{
		snprintfResult =
			snprintf(
				AoHeaderCheckErrorStr,
				MAX_AOHEADER_CHECK_ERROR_STR,
				"Append-only storage header is invalid -- reserved bit 0 of the header is not zero (header_bytes_0_3 0x%08x, header_bytes_4_7 0x%08x)",
			    header->header_bytes_0_3, header->header_bytes_4_7);
		
		Assert(snprintfResult >= 0);
		Assert(snprintfResult < MAX_AOHEADER_CHECK_ERROR_STR);
		
		return AOHeaderCheckReservedBit0Not0;
	}

	*headerKind = AOHeaderGet_headerKind(header);

	if (*headerKind == AoHeaderKind_None)
	{
		snprintfResult =
			snprintf(
				AoHeaderCheckErrorStr,
				MAX_AOHEADER_CHECK_ERROR_STR,
				"Append-only storage header is invalid -- invalid value 0 (none) for header kind (header_bytes_0_3 0x%08x, header_bytes_4_7 0x%08x)",
			    header->header_bytes_0_3, header->header_bytes_4_7);
		
		Assert(snprintfResult >= 0);
		Assert(snprintfResult < MAX_AOHEADER_CHECK_ERROR_STR);
		
		return AOHeaderCheckInvalidHeaderKindNone;
	}

	if (*headerKind >= MaxAoHeaderKind)
	{
		snprintfResult =
			snprintf(
				AoHeaderCheckErrorStr,
				MAX_AOHEADER_CHECK_ERROR_STR,
				"Append-only storage header is invalid -- invalid header kind value %d (header_bytes_0_3 0x%08x, header_bytes_4_7 0x%08x)",
				(int)*headerKind,
			    header->header_bytes_0_3, header->header_bytes_4_7);
		
		Assert(snprintfResult >= 0);
		Assert(snprintfResult < MAX_AOHEADER_CHECK_ERROR_STR);
		
		return AOHeaderCheckInvalidHeaderKind;
	}

	switch (*headerKind)
	{
	case AoHeaderKind_SmallContent:
		{
			AOBlockHeader	*blockHeader;

			blockHeader = (AOBlockHeader*)headerPtr;

			*actualHeaderLen = 
						AO_HEADER_LEN + 
						(usingChecksums ? 2 * sizeof(pg_crc32) : 0);
			if (AOBlockHeaderGet_hasFirstRowNum(blockHeader))
				(*actualHeaderLen) += sizeof(int64);
		}
		break;
		
	case AoHeaderKind_LargeContent:
		{
			AOLargeContentHeader	*largeContentHeader;

			largeContentHeader = (AOLargeContentHeader*)headerPtr;

			*actualHeaderLen = 
						AO_HEADER_LEN + 
						(usingChecksums ? 2 * sizeof(pg_crc32) : 0);
			if (AOLargeContentHeaderGet_hasFirstRowNum(largeContentHeader))
				(*actualHeaderLen) += sizeof(int64);
		}
		break;

	default:
		// UNDONE:
		break;
	}	
	
	return AOHeaderCheckOk;
}

AOHeaderCheckError
AppendOnlyStorageFormat_GetSmallContentHeaderInfo(
	uint8			*headerPtr,
	int				headerLen,
	bool			usingChecksums,
	int32			blockLimitLen,
	int32			*overallBlockLen,
	int32			*offset,
	int32			*uncompressedLen,
	int				*executorBlockKind,
	bool			*hasFirstRowNum,
	int				version,
	int64			*firstRowNum,
	int				*rowCount,
	bool			*isCompressed,
	int32			*compressedLen)
{
	AOBlockHeader 	*blockHeader;
	int32			length;

	Assert(headerPtr != NULL);

	blockHeader = (AOBlockHeader*)headerPtr;

	*executorBlockKind = 	AOBlockHeaderGet_executorBlockKind(blockHeader);
	*hasFirstRowNum	=		AOBlockHeaderGet_hasFirstRowNum(blockHeader);
	*rowCount = 			AOBlockHeaderGet_rowCount(blockHeader);	

	*offset =				AO_HEADER_LEN + 
							(usingChecksums ? 2 * sizeof(pg_crc32) : 0);
	if (*hasFirstRowNum)
	{
		int64	*firstRowNumPtr;

		firstRowNumPtr = (int64*)&headerPtr[*offset];
		*firstRowNum = *firstRowNumPtr;
		
		(*offset) += sizeof(int64);
	}
	else
		*firstRowNum = INT64CONST(-1);
	
	if (*offset != headerLen)
		elog(ERROR, "Content offset %d doesn't equal header length parameter %d",
		     *offset,
		     headerLen);
	
	*uncompressedLen =	AOBlockHeaderGet_dataLength(blockHeader);
	*compressedLen = 	AOBlockHeaderGet_compressedLength(blockHeader);
	if (*compressedLen == 0)
	{
		*isCompressed = 	false;
		length =			*uncompressedLen;
	}
	else
	{
		*isCompressed = 	true;
		length =			*compressedLen;

		/*
		 * UNDONE: Fix doCompressAppend to supply slightly less output buffer...
		 * UNDONE: so we can make this comparison >=.
		 */
		if (*compressedLen > *uncompressedLen)
		{
			sprintf(AoHeaderCheckErrorStr,
					"Append-only storage header is invalid -- compressed length %d is > uncompressed length %d "
					"(block_bytes_0_3 0x%08x, block_bytes_4_7 0x%08x)",
					*compressedLen,
					*uncompressedLen,
				    blockHeader->block_bytes_0_3, blockHeader->block_bytes_4_7);
			return AOHeaderCheckInvalidCompressedLen;
		}
	}
	
	*overallBlockLen =		*offset +
	                        AOStorage_RoundUp(length, version);

	if (*overallBlockLen > blockLimitLen)
	{
		sprintf(AoHeaderCheckErrorStr,
				"Append-only storage header is invalid -- overall block length %d is > block limit length %d "
				"(block_bytes_0_3 0x%08x, block_bytes_4_7 0x%08x)",
				*overallBlockLen,
				blockLimitLen,
			    blockHeader->block_bytes_0_3, blockHeader->block_bytes_4_7);
		return AOHeaderCheckInvalidOverallBlockLen;
	}
	
	return AOHeaderCheckOk;
}

int32
AppendOnlyStorageFormat_GetUncompressedLen(
	uint8			*headerPtr)
{
	AOBlockHeader 	*blockHeader;

	Assert(headerPtr != NULL);
	blockHeader = (AOBlockHeader*)headerPtr;
	Assert(blockHeader->block_bytes_0_3 != 0);

	return AOBlockHeaderGet_dataLength(blockHeader);
}

int32
AppendOnlyStorageFormat_GetCompressedLen(
	uint8			*headerPtr)
{
	AOBlockHeader 	*blockHeader;

	Assert(headerPtr != NULL);
	blockHeader = (AOBlockHeader*)headerPtr;
	Assert(blockHeader->block_bytes_0_3 != 0);

	return AOBlockHeaderGet_compressedLength(blockHeader);
}

AOHeaderCheckError
AppendOnlyStorageFormat_GetLargeContentHeaderInfo(
	uint8			*headerPtr,
	int				headerLen,
	bool			usingChecksums,
	int32			*largeContentLen,
	int				*executorBlockKind,
	bool			*hasFirstRowNum,
	int64			*firstRowNum,
	int				*largeRowCount)
{
	AOLargeContentHeader 	*largeContentHeader;
	int32 offset;

	Assert(headerPtr != NULL);

	largeContentHeader = (AOLargeContentHeader*)headerPtr;

	*executorBlockKind = 	AOLargeContentHeaderGet_executorBlockKind(largeContentHeader);
	*hasFirstRowNum	=		AOLargeContentHeaderGet_hasFirstRowNum(largeContentHeader);
	*largeRowCount = 		AOLargeContentHeaderGet_largeRowCount(largeContentHeader);
	*largeContentLen = 		AOLargeContentHeaderGet_largeContentLength(largeContentHeader);
	if (*largeContentLen == 0)
	{
		sprintf(AoHeaderCheckErrorStr,
				"Append-only storage header is invalid -- large content length is zero "
				"(block_bytes_0_3 0x%08x, block_bytes_4_7 0x%08x)",
			    largeContentHeader->largecontent_bytes_0_3, largeContentHeader->largecontent_bytes_4_7);
		return AOHeaderCheckLargeContentLenIsZero;
	}
	
	offset = AO_HEADER_LEN + 
			 (usingChecksums ? 2 * sizeof(pg_crc32) : 0);
	if (*hasFirstRowNum)
	{
		int64	*firstRowNumPtr;

		firstRowNumPtr = (int64*)&headerPtr[offset];
		*firstRowNum = *firstRowNumPtr;
		
		offset += sizeof(int64);
	}
	else
		*firstRowNum = INT64CONST(-1);

	if (offset != headerLen)
		elog(ERROR, "Content offset %d doesn't equal header length parameter %d",
		     offset,
		     headerLen);

	return AOHeaderCheckOk;
}


bool
AppendOnlyStorageFormat_VerifyHeaderChecksum(
	uint8			*headerPtr,
	pg_crc32		*storedChecksum,
	pg_crc32		*computedChecksum)
{
	int32			headerLen;

	pg_crc32	*headerChecksumPtr;
	
	Assert(headerPtr != NULL);
	Assert(storedChecksum != NULL);
	Assert(computedChecksum != NULL);
	headerLen = AO_HEADER_LEN +
				sizeof(pg_crc32);		// Block checksum.

	/*
	 * CRC checksum is first 32 bits after the whole header.
	 */
	headerChecksumPtr = (pg_crc32*)&headerPtr[headerLen];
	*storedChecksum = *headerChecksumPtr;
	
	*computedChecksum = AppendOnlyStorageFormat_ComputeHeaderChecksum(
														headerPtr,
														headerLen);

	return (*storedChecksum == *computedChecksum);	
}

bool
AppendOnlyStorageFormat_VerifyBlockChecksum(
	uint8			*headerPtr,
	int32			overallBlockLen,
	pg_crc32		*storedChecksum,
	pg_crc32		*computedChecksum)
{
	pg_crc32	*blockChecksumPtr;
	
	Assert(headerPtr != NULL);
	Assert(storedChecksum != NULL);
	Assert(computedChecksum != NULL);

	/*
	 * Block checksum is first 32 bits after header.
	 */
	blockChecksumPtr = (pg_crc32*)&headerPtr[AO_HEADER_LEN];
	*storedChecksum = *blockChecksumPtr;
	
	*computedChecksum = AppendOnlyStorageFormat_ComputeBlockChecksum(
														headerPtr,
														AO_HEADER_LEN + sizeof(pg_crc32),
														overallBlockLen);

	return (*storedChecksum == *computedChecksum);	
}
