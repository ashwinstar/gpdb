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

#include "utils/pg_crc.h"


#define AOLARGE_HEADER_LEN 8	// 64 bits.

#define AOLargeHeader_MaxRowCount 0x3FFF   // 14 bits, or 16,383 (16k – 1).
#define AOLargeHeader_MaxLength   0x1FFFFF // 21 bits, or 2,097,151 (2 Mb – 1).


#define AOSMALL_HEADER_LEN 4	// 32 bits.

#define AOSmallHeader_MaxRowCount 0x3FF    // 10 bits, or 1,023 (1k – 1).
#define AOSmallHeader_MaxLength   0xFFFF   // 16 bits, or 65,535 (64k – 1).

#define AOHeader_CanBeSmall(r,l) ((r)<= AOSmallHeader_MaxRowCount&&(l)<= AOSmallHeader_MaxLength)

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


typedef enum AOHeaderCheckError
{
	AOHeaderCheckOk = 0,
	AOHeaderCheckAllZeroes,
} AOHeaderCheckError;


typedef struct AOTupleId
{
/*
 * Can't seem to get this to pack nicely to 48 bits so the header is aligned
 * on an 4 byte boundary, so go to bit shifting...
 */

//  unsigned          reserved0:1;  
//  unsigned          reserved1:1;
//						  /* Two reserved bits */
//
//  unsigned          segmentFileNum:10;  
//                        /*
//                         * Which segment file the tuple is in.  
//						   * Limit: 1023.
//                         */
//  unsigned          rowNumber:36;
//                         /* 
//                          * The row within the segment file.
//							* Limit: 68 Trillion (2^36) - 1.
//                          */

    uint16            bytes_0_1;
    uint32            bytes_2_5;

} AOTupleId;

#define AOTupleIdGet_reserved0(h)             ((h)->bytes_0_1>>15)                 // 1 bit
#define AOTupleIdGet_reserved1(h)             (((h)->bytes_0_1&0x4000)>>14)       // 1 bit
#define AOTupleIdGet_segmentFileNum(h)        ((h)->bytes_0_1&0x3FF0>>4)          // 10 bits
#define AOTupleIdGet_rowNum(h) \
          ((((uint64)((h)->bytes_0_1)&0x000F)<<32)|((uint64)(h)->bytes_2_5)
                         // top 4 bits                   all lower 32 bits          36 bits
                         

// For single bits, set or clear directly.  Otherwise, use AND to clear field then OR to set.
#define AOTupleIdSet_reserved0(h,e)  {if(e)(h)->bytes_0_1|=0x8000;else(h)->bytes_0_1&=0x7FFF;} 
#define AOTupleIdSet_reserved1(h,e)  {if(e)(h)->bytes_0_1|=0x4000;else(h)->bytes_0_1&=0xBFFF;} 
#define AOTupleIdSet_segmentFileNum(h,e)         {(h)->bytes_0_1&=0xC00F;(h)->bytes_0_1|=((0x03FF&((uint16)(e)))<<4);}
#define AOTupleIdSet_rowNum(h,e)    {(h)->bytes_0_1&=0xFFF0;(h)->bytes_0_1|=(uint16)((0x0000000F00000000&(e))>>32);\
                                     (h)->bytes_2_5=(uint32)(e);}

// Assume field is initially zero.
#define AOTupleIdInit_Init(h)                {(h)->bytes_0_1=0;(h)->bytes_2_5=0;} 

#define AOTupleIdInit_reserved0(h,e)  {if(e)(h)->bytes_0_1|=0x8000;} 
#define AOTupleIdInit_reserved1(h,e)  {if(e)(h)->bytes_0_1|=0x4000;} 
#define AOTupleIdInit_segmentFileNum(h,e)    {(h)->bytes_0_1|=((0x03FF&((uint16)(e)))<<4);}
#define AOTupleIdInit_rowNum(h,e)            {(h)->bytes_0_1|=(0x000F&((uint16)((e)>>32)));\
                                              (h)->bytes_2_5=(uint32)(e);}



extern int32 AppendOnlyStorage_HeaderLenNeeded(
	bool			wantLarge,
	bool			wantChecksum);

extern void AppendOnlyStorage_MakeLongHeader(
	uint8			*headerPtr,
	bool			wantChecksum,
	int				applicationSpecific,
	int				rowCount,
	int32			dataLength,
	int32			compressedLength);

extern void AppendOnlyStorage_MakeShortHeader(
	uint8			*headerPtr,
	bool			wantChecksum,
	int				applicationSpecific,
	int				rowCount,
	int32			dataLength);

extern void AppendOnlyStorage_LogHeader(
	char			*tableName,
	int				segmentFileNum,
	int64			headerOffsetInFile,
	uint8			*headerPtr);

extern AOHeaderCheckError AppendOnlyStorage_GetHeaderInfo(
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
	bool			*isCompressed);

extern bool AppendOnlyStorage_VerifyChecksum(
	uint8			*headerPtr,
	int32			overallBlockLen,
	pg_crc32		*storedChecksum,
	pg_crc32		*computedChecksum);

extern int32 AppendOnlyStorage_GetUncompressedLen(
	uint8			*headerPtr);

#endif   /* APPENDONLYSTORAGE_H */
