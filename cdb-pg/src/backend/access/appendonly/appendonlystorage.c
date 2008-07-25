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
AppendOnlyStorage_ComputeChecksum(
	uint8			*headerPtr,
	int32			headerLen,
	int32			overallBlockLen)
{
	pg_crc32	*checksumPtr;
	pg_crc32	crc;
	
	Assert(headerPtr != NULL);

	/*
	 * CRC checksum is first 32 bits after the small or large header.
	 */
	checksumPtr = (pg_crc32*)&headerPtr[headerLen];

	INIT_CRC32(crc);

	/* Compute CRC of the header. */
	COMP_CRC32(crc, 
	           headerPtr, 
	           headerLen);

	/* Compute CRC of all headers and data after the CRC. */
	COMP_CRC32(crc, 
	           &checksumPtr[1], 
	           overallBlockLen - sizeof(pg_crc32) - headerLen);

	return crc;
}

int32 
AppendOnlyStorage_HeaderLenNeeded(
	bool			wantLarge,
	bool			wantChecksum)
{
	return (wantLarge ? AOLARGE_HEADER_LEN : AOSMALL_HEADER_LEN ) +
		   (wantChecksum ? sizeof(pg_crc32) : 0);
}

void
AppendOnlyStorage_MakeLongHeader(
	uint8			*headerPtr,
	bool			wantChecksum,
	int				applicationSpecific,
	int				rowCount,
	int32			dataLength,
	int32			compressedLength)
{
	AOLargeHeader 	*largeHeader;
	bool			isCompressed;

	Assert(headerPtr != NULL);
	
	largeHeader = (AOLargeHeader*)headerPtr;

	if (Debug_appendonly_print_storage_headers)
		elog(LOG,
			 "Append-Only storage make long header parameters: wantChecksum = %s, applicationSpecific = %d, "
			 "rowCount = %d, dataLength %d, compressedLength %d",
			 (wantChecksum ? "true" : "false"),
			 applicationSpecific,
			 rowCount,
			 dataLength,
			 compressedLength);
	
	/* Zero out whole header */
	AOLargeHeaderInit_Init(largeHeader);
	
	AOLargeHeaderInit_isLargeheader(largeHeader,true);
	if (wantChecksum)
		AOLargeHeaderInit_hasChecksum(largeHeader,true);
	// UNDONE: kind
	AOLargeHeaderInit_applicationSpecific(largeHeader,applicationSpecific);
	AOLargeHeaderInit_rowCount(largeHeader,rowCount);
	AOLargeHeaderInit_dataLength(largeHeader,dataLength);

	isCompressed = (compressedLength > 0);
	if (isCompressed)
		AOLargeHeaderInit_compressedLength(largeHeader,compressedLength);

	if (wantChecksum)
	{
		int32		overallBlockLen;
		pg_crc32	*checksumPtr;
		
		overallBlockLen = AOLARGE_HEADER_LEN + 
						  sizeof(pg_crc32) +
		                  AOStorage_RoundUp(
		                        (isCompressed ? compressedLength : dataLength));
		/*
		 * CRC checksum is first 32 bits after small/large header.
		 */
		checksumPtr = (pg_crc32*)&headerPtr[AOLARGE_HEADER_LEN];
		*checksumPtr = AppendOnlyStorage_ComputeChecksum(
														headerPtr,
														AOLARGE_HEADER_LEN,
														overallBlockLen);
		if (Debug_appendonly_print_storage_headers)
			elog(LOG,
				 "Append-Only storage make long header result: large_bytes_0_3 0x%X, large_bytes_4_7 0x%X, "
				 "checksum 0x%X, overallBlockLen %d",
				 largeHeader->large_bytes_0_3,
				 largeHeader->large_bytes_4_7,
				 *checksumPtr,
				 overallBlockLen);
	}
	else
	{
		if (Debug_appendonly_print_storage_headers)
			elog(LOG,
				 "Append-Only storage make long header result: large_bytes_0_3 0x%X, large_bytes_4_7 0x%X",
				 largeHeader->large_bytes_0_3,
				 largeHeader->large_bytes_4_7);
	}
}

void
AppendOnlyStorage_MakeShortHeader(
	uint8			*headerPtr,
	bool			wantChecksum,
	int				applicationSpecific,
	int				rowCount,
	int32			dataLength)
{
	AOSmallHeader 	*smallHeader;

	Assert(headerPtr != NULL);
	
	smallHeader = (AOSmallHeader*)headerPtr;

	if (Debug_appendonly_print_storage_headers)
		elog(LOG,
			 "Append-Only storage make short header parameters: wantChecksum = %s, applicationSpecific = %d, "
			 "rowCount = %d, dataLength %d",
			 (wantChecksum ? "true" : "false"),
			 applicationSpecific,
			 rowCount,
			 dataLength);
	
	/* Zero out whole header */
	AOSmallHeaderInit_Init(smallHeader);
	// isLarge = false
	if (wantChecksum)
		AOSmallHeaderInit_hasChecksum(smallHeader,true);
	// UNDONE: kind
	AOSmallHeaderInit_applicationSpecific(smallHeader,applicationSpecific);
	AOSmallHeaderInit_rowCount(smallHeader,rowCount);
	AOSmallHeaderInit_dataLength(smallHeader,dataLength);

	if (wantChecksum)
	{
		int32		overallBlockLen;
		pg_crc32	*checksumPtr;
		
		overallBlockLen = AOSMALL_HEADER_LEN + 
						  sizeof(pg_crc32) +
		                  AOStorage_RoundUp(dataLength);
		/*
		 * CRC checksum is first 32 bits after small/large header.
		 */
		checksumPtr = (pg_crc32*)&headerPtr[AOSMALL_HEADER_LEN];
		*checksumPtr = AppendOnlyStorage_ComputeChecksum(
														headerPtr,
														AOSMALL_HEADER_LEN,
														overallBlockLen);
		if (Debug_appendonly_print_storage_headers)
			elog(LOG,
				 "Append-Only storage make small header result: bytes_0_3 0x%X, "
				 "checksum 0x%X, overallBlockLen %d",
				 smallHeader->bytes_0_3,
				 *checksumPtr,
				 overallBlockLen);
	}
	else
	{
		if (Debug_appendonly_print_storage_headers)
			elog(LOG,
				 "Append-Only storage make small header result: bytes_0_3 0x%X",
				 smallHeader->bytes_0_3);
	}
}
void
AppendOnlyStorage_LogHeader(
	char			*tableName,
	int				segmentFileNum,
	int64			headerOffsetInFile,
	uint8			*headerPtr)
{
	AOLargeHeader 	*largeHeader;
	int32			headerLen;
	int32			wholeHeaderLen;

	bool		isLarge;
	int			applicationSpecific;
	int			rowCount;
//	bool		isFragmented;
	bool		hasChecksum;
	int32		dataLength;
	int32		compressedLength;
	int32		overallBlockLen;

	pg_crc32	*checksumPtr;
	pg_crc32	checksum;

	Assert(headerPtr != NULL);

	largeHeader = (AOLargeHeader*)headerPtr;

	if (!AOLargeHeaderGet_isLargeHeader(largeHeader))
	{
		AOSmallHeader *smallHeader;
		
		smallHeader = (AOSmallHeader*)headerPtr;
		headerLen = AOSMALL_HEADER_LEN;
		isLarge = false;
		
		applicationSpecific = 	AOSmallHeaderGet_applicationSpecific(smallHeader);
		rowCount = 				AOSmallHeaderGet_rowCount(smallHeader);
//		isFragmented = 			false;
		hasChecksum = 			AOSmallHeaderGet_hasChecksum(smallHeader);

		// UNDONE: Add extra fragment information length.
		wholeHeaderLen =		headerLen + 
								(hasChecksum ? sizeof(pg_crc32) : 0);
		
		dataLength =			AOSmallHeaderGet_dataLength(smallHeader);

		compressedLength =		0;

		overallBlockLen =		wholeHeaderLen +
		                        AOStorage_RoundUp(dataLength);
	}
	else
	{
		headerLen = AOLARGE_HEADER_LEN;
		isLarge = true;

		applicationSpecific = 	AOLargeHeaderGet_applicationSpecific(largeHeader);
		rowCount = 				AOLargeHeaderGet_rowCount(largeHeader);
//		isFragmented = 			false;	// UNDONE: For now, we haven't added fragment support for large headers yet...
		hasChecksum = 			AOLargeHeaderGet_hasChecksum(largeHeader);
		
		// UNDONE: Add extra fragment information length.
		wholeHeaderLen =		headerLen + 
								(hasChecksum ? sizeof(pg_crc32) : 0);
		
		dataLength =			AOLargeHeaderGet_dataLength(largeHeader);
		
		compressedLength =		AOLargeHeaderGet_compressedLength(largeHeader);

		if (compressedLength == 0)
			overallBlockLen =		wholeHeaderLen +
			                        AOStorage_RoundUp(dataLength);
		else
			overallBlockLen =		wholeHeaderLen +
			                        AOStorage_RoundUp(compressedLength);
	}

	if (hasChecksum)
	{
		checksumPtr = (pg_crc32*)&headerPtr[headerLen];
		checksum = *checksumPtr;
	}
	else
		checksum = 0;

	if (!isLarge)
		elog(LOG,
			 "Append-Only storage short header: table %s, segment file #%d, header offset in file " INT64_FORMAT ", "
			 "large_bytes_0_3 0x%X, large_bytes_4_7 0x%X, "
			 "applicationSpecific = %d, "
			 "rowCount = %d, hasChecksum = %s, checksum 0x%X, "
			 "dataLength %d, overallBlockLen %d",
			 tableName,
			 segmentFileNum,
			 headerOffsetInFile,
			 largeHeader->large_bytes_0_3,
			 largeHeader->large_bytes_4_7,
			 applicationSpecific,
			 rowCount,
			 (hasChecksum ? "true" : "false"),
			 checksum,
			 dataLength,
			 overallBlockLen);
	else
		elog(LOG,
			 "Append-Only storage long header: table %s, segment file #%d, header offset in file " INT64_FORMAT ", "
			 "large_bytes_0_3 0x%X, large_bytes_4_7 0x%X, "
			 "applicationSpecific = %d, "
			 "rowCount = %d, hasChecksum = %s, checksum 0x%X, "
			 "dataLength %d, compressedLength %d, overallBlockLen %d",
			 tableName,
			 segmentFileNum,
			 headerOffsetInFile,
			 largeHeader->large_bytes_0_3,
			 largeHeader->large_bytes_4_7,
			 applicationSpecific,
			 rowCount,
			 (hasChecksum ? "true" : "false"),
			 checksum,
			 dataLength,
			 compressedLength,
			 overallBlockLen);
}

AOHeaderCheckError
AppendOnlyStorage_GetHeaderInfo(
	uint8			*headerPtr,
	int				headerLen,
	int64			blockLimitLen,
	int32			*overallBlockLen,
	int32			*offset,
	int32			*length,
	int				*applicationSpecific,
	int				*rowCount,
	bool			*isFragmented,
	bool			*hasChecksum,
	bool			*isCompressed)
{
	AOLargeHeader 	*largeHeader;

	Assert(headerPtr != NULL);
	Assert(headerLen == AOLARGE_HEADER_LEN);

	largeHeader = (AOLargeHeader*)headerPtr;

	if (largeHeader->large_bytes_0_3 == 0)
		return AOHeaderCheckAllZeroes;

	if (!AOLargeHeaderGet_isLargeHeader(largeHeader))
	{
		AOSmallHeader *smallHeader;
		
		smallHeader = (AOSmallHeader*)headerPtr;
		
		// UNDONE: Validation checks

		*applicationSpecific = 	AOSmallHeaderGet_applicationSpecific(smallHeader);
		*rowCount = 			AOSmallHeaderGet_rowCount(smallHeader);
		*isFragmented = 		false;
		*hasChecksum = 			AOSmallHeaderGet_hasChecksum(smallHeader);
		*isCompressed = 		false;

		// UNDONE: Add extra fragment information length.
		*offset =				AOSMALL_HEADER_LEN + 
								(*hasChecksum ? sizeof(pg_crc32) : 0);
		
		*length =				AOSmallHeaderGet_dataLength(smallHeader);

		*overallBlockLen =		*offset +
		                        AOStorage_RoundUp(*length);

		return AOHeaderCheckOk;
	}

	// UNDONE: Validation checks

	*applicationSpecific = 	AOLargeHeaderGet_applicationSpecific(largeHeader);
	*rowCount = 			AOLargeHeaderGet_rowCount(largeHeader);
	*isFragmented = 		false;	// UNDONE: For now, we haven't added fragment support for large headers yet...
	*hasChecksum = 			AOLargeHeaderGet_hasChecksum(largeHeader);
	*isCompressed = 		(AOLargeHeaderGet_compressedLength(largeHeader) > 0);
	
	// UNDONE: Add extra fragment information length.
	*offset =				AOLARGE_HEADER_LEN + 
							(*hasChecksum ? sizeof(pg_crc32) : 0);
	
	if (!*isCompressed)
		*length =			AOLargeHeaderGet_dataLength(largeHeader);
	else
		*length =			AOLargeHeaderGet_compressedLength(largeHeader);
	
	*overallBlockLen =		*offset +
	                        AOStorage_RoundUp(*length);

	return AOHeaderCheckOk;
}

int32
AppendOnlyStorage_GetUncompressedLen(
	uint8			*headerPtr)
{
	AOLargeHeader 	*largeHeader;

	Assert(headerPtr != NULL);
	largeHeader = (AOLargeHeader*)headerPtr;
	Assert(largeHeader->large_bytes_0_3 != 0);
	Assert(AOLargeHeaderGet_isLargeHeader(largeHeader));

	return AOLargeHeaderGet_dataLength(largeHeader);
}

bool
AppendOnlyStorage_VerifyChecksum(
	uint8			*headerPtr,
	int32			overallBlockLen,
	pg_crc32		*storedChecksum,
	pg_crc32		*computedChecksum)
{
	AOLargeHeader 	*largeHeader;
	int32			headerLen;

	pg_crc32	*checksumPtr;
	
	Assert(headerPtr != NULL);
	Assert(storedChecksum != NULL);
	Assert(computedChecksum != NULL);
	largeHeader = (AOLargeHeader*)headerPtr;
	headerLen = (AOLargeHeaderGet_isLargeHeader(largeHeader) ? 
													AOLARGE_HEADER_LEN :
													AOSMALL_HEADER_LEN);

	/*
	 * CRC checksum is first 32 bits after small/large header.
	 */
	checksumPtr = (pg_crc32*)&headerPtr[headerLen];
	*storedChecksum = *checksumPtr;
	
	*computedChecksum = AppendOnlyStorage_ComputeChecksum(
													headerPtr,
													headerLen,
													overallBlockLen);

	return (*storedChecksum == *computedChecksum);	
}

