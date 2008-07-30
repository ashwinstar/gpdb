/*-------------------------------------------------------------------------
 *
 * appendonlystorage.c
 *
 * Copyright (c) 2007-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/appendonlystorage_int.h"
#include "access/appendonlystorage.h"
#include "utils/pg_crc.h"
#include "utils/guc.h"

static pg_crc32
AppendOnlyStorage_ComputeHeaderChecksum(
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

	return crc;
}

static pg_crc32
AppendOnlyStorage_ComputeBlockChecksum(
	uint8			*headerPtr,
	int32			headerLen,
	int32			overallBlockLen)
{
	int32		dataOffset;
	pg_crc32	crc;
	
	Assert(headerPtr != NULL);

	/*
	 * The block checksum covers right after the header checksum through
	 * the end of the whole block.
	 */
	dataOffset = headerLen + sizeof(pg_crc32);

	INIT_CRC32(crc);

	/* Compute CRC of the header. */
	COMP_CRC32(crc, 
	           headerPtr + dataOffset, 
	           overallBlockLen - dataOffset);

	return crc;
}


int32 
AppendOnlyStorage_HeaderLenNeeded(
	bool			usingChecksum)
{
	return AO_HEADER_LEN +
		   (usingChecksum ? 2 * sizeof(pg_crc32) : 0);
}

static void
AppendOnlyStorage_AddBlockHeaderChecksums(
	uint8			*headerPtr,
	bool			isCompressed,
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
					  sizeof(pg_crc32) +	// Header checksum.
	                  AOStorage_RoundUp(
	                        (isCompressed ? compressedLength : dataLength));
	
	/*
	 * Calculate block checksum first since it is included in the 
	 * header checksum.
	 */
	blockChecksumPtr = (pg_crc32*)&headerPtr[AO_HEADER_LEN];
	*blockChecksumPtr = AppendOnlyStorage_ComputeBlockChecksum(
													headerPtr,
													headerLen, 
													overallBlockLen);
	/*
	 * Now the Header checksum after the whole header.
	 */
	headerChecksumPtr = (pg_crc32*)&headerPtr[headerLen];
	*headerChecksumPtr = AppendOnlyStorage_ComputeHeaderChecksum(
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
AppendOnlyStorage_MakeBlockHeader(
	uint8			*headerPtr,
	bool			usingChecksums,
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
			 "Append-Only storage make block header parameters: wantChecksum = %s, executorKind = %d, "
			 "rowCount = %d, dataLength %d, compressedLength %d",
			 (usingChecksums ? "true" : "false"),
			 executorKind,
			 rowCount,
			 dataLength,
			 compressedLength);
	
	/* Zero out whole header */
	AOBlockHeaderInit_Init(blockHeader);
	
	AOBlockHeaderInit_headerKind(blockHeader,AoHeaderKind_Block);
	AOBlockHeaderInit_executorBlockKind(blockHeader,executorKind);
	AOBlockHeaderInit_rowCount(blockHeader,rowCount);
	AOBlockHeaderInit_dataLength(blockHeader,dataLength);

	isCompressed = (compressedLength > 0);
	if (isCompressed)
		AOBlockHeaderInit_compressedLength(blockHeader,compressedLength);

	if (usingChecksums)
	{
		AppendOnlyStorage_AddBlockHeaderChecksums(
											headerPtr,
											isCompressed,
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
AppendOnlyStorage_LogBlockHeader(
	char			*tableName,
	int				segmentFileNum,
	int64			headerOffsetInFile,
	uint8			*headerPtr,
	bool			usingChecksums)
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
	                        	(compressedLength == 0 ? dataLength : compressedLength));

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
		 "Append-Only storage long header: table %s, segment file #%d, header offset in file " INT64_FORMAT ", "
		 "block_bytes_0_3 0x%X, block_bytes_4_7 0x%X, "
		 "executorBlockKind = %d, "
		 "rowCount = %d, usingChecksums = %s, header checksum 0x%X, block checksum 0x%X, "
		 "dataLength %d, compressedLength %d, overallBlockLen %d",
		 tableName,
		 segmentFileNum,
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

AOHeaderCheckError
AppendOnlyStorage_GetBlockHeaderInfo(
	uint8			*headerPtr,
	int				headerLen,
	bool			usingChecksums,
	int64			blockLimitLen,
	int32			*overallBlockLen,
	int32			*offset,
	int32			*length,
	int				*executorBlockKind,
	int				*rowCount,
	bool			*isCompressed)
{
	AOBlockHeader 	*blockHeader;

	Assert(headerPtr != NULL);

	blockHeader = (AOBlockHeader*)headerPtr;

	if (blockHeader->block_bytes_0_3 == 0)
		return AOHeaderCheckAllZeroes;

	// UNDONE: Validation checks

	*executorBlockKind = 	AOBlockHeaderGet_executorBlockKind(blockHeader);
	*rowCount = 			AOBlockHeaderGet_rowCount(blockHeader);
	*isCompressed = 		(AOBlockHeaderGet_compressedLength(blockHeader) > 0);
	
	*offset =				AO_HEADER_LEN + 
							(usingChecksums ? 2 * sizeof(pg_crc32) : 0);
	
	if (!*isCompressed)
		*length =			AOBlockHeaderGet_dataLength(blockHeader);
	else
		*length =			AOBlockHeaderGet_compressedLength(blockHeader);
	
	*overallBlockLen =		*offset +
	                        AOStorage_RoundUp(*length);

	return AOHeaderCheckOk;
}

int32
AppendOnlyStorage_GetUncompressedLen(
	uint8			*headerPtr)
{
	AOBlockHeader 	*blockHeader;

	Assert(headerPtr != NULL);
	blockHeader = (AOBlockHeader*)headerPtr;
	Assert(blockHeader->block_bytes_0_3 != 0);

	return AOBlockHeaderGet_dataLength(blockHeader);
}

bool
AppendOnlyStorage_VerifyHeaderChecksum(
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
	
	*computedChecksum = AppendOnlyStorage_ComputeHeaderChecksum(
														headerPtr,
														headerLen);

	return (*storedChecksum == *computedChecksum);	
}

bool
AppendOnlyStorage_VerifyBlockChecksum(
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
	
	*computedChecksum = AppendOnlyStorage_ComputeBlockChecksum(
														headerPtr,
														AO_HEADER_LEN + sizeof(pg_crc32),
														overallBlockLen);

	return (*storedChecksum == *computedChecksum);	
}


