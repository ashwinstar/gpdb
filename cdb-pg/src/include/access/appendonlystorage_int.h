/*-------------------------------------------------------------------------
 *
 * appendonlystorage.h
 *
 * Copyright (c) 2007-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef APPENDONLYSTORAGE_INT_H
#define APPENDONLYSTORAGE_INT_H

typedef struct AOLargeHeader
{
/*
 * Can't seem to get this to pack nicely to 64 bits so the header is aligned
 * on an 8 byte boundary, so go to bit shifting...
 */

//  unsigned          isLargeHeader:1;  
//                        /*
//                         * True if this header is the long 64 bit AO
//                         */
//  unsigned          hasChecksum:1;  
//                        /*
//                         * True if there is a 32 bit CRC checksum of the block.
//                         */
//  unsigned          reserved:1;  
//                        /* 
//                         * Reserved for future use.
//                         */
//  AOLargeHeaderKind kind:2;  
//                        /* 
//                         * Which kind of block is represented by this large
//                         * header.
//                         */
//  unsigned          applicationSpecific:3;  
//                         /* 
//                          * Application (i.e. executor) 3 bit value.  
//                          */
//
//  unsigned          rowCount:14;
//                         /* 
//                          * The number of rows in the block. Maintained in the header so
//                          * blocks can be skipped without decompression, etc.
//                          */
//
//  unsigned          dataLength:21;
//                         /* Length of the data for non-compressed blocks, or
//                          * the uncompressed length for compressed blocks.
//                          */
//  unsigned          compressedLength:21;
//                         /* Length of the block s compressed data; 
//                          * or 0 if non-compressed.
//                          */

    uint32            large_bytes_0_3;
    uint32            large_bytes_4_7;

} AOLargeHeader;

#define AOLargeHeaderGet_isLargeHeader(h)  ((h)->large_bytes_0_3>>31)                  // 1 bit
#define AOLargeHeaderGet_hasChecksum(h)    (((h)->large_bytes_0_3&0x40000000)>>30)    // 1 bit
#define AOLargeHeaderGet_isReserved1(h)    (((h)->large_bytes_0_3&0x20000000)>>29)    // 1 bit
#define AOLargeHeaderGet_kind(h)           (((h)->large_bytes_0_3&0x18000000)>>27)    // 2 bits
#define AOLargeHeaderGet_applicationSpecific(h)    \
	                                       (((h)->large_bytes_0_3&0x07000000)>>24)    // 3 bits
#define AOLargeHeaderGet_rowCount(h)       (((h)->large_bytes_0_3&0x00FFFC00)>>10)    // 14 bits
#define AOLargeHeaderGet_dataLength(h) \
          ((((h)->large_bytes_0_3&0x000003FF)<<11)|(((h)->large_bytes_4_7&0xFFE00000)>>21))
                         // top 10 bits                      lower 11 bits               21 bits
#define AOLargeHeaderGet_compressedLength(h) ((h)->large_bytes_4_7&0x001FFFFF)        // 21 bits

// For single bits, set or clear directly.  Otherwise, use AND to clear field then OR to set.
#define AOLargeHeaderSet_isLargeHeader(h,e) {if(e)(h)->large_bytes_0_3|=0x80000000;else(h)->large_bytes_0_3&=0x7FFFFFFF;} 
#define AOLargeHeaderSet_hasChecksum(h,e)  {if(e)(h)->large_bytes_0_3|=0x40000000;else(h)->large_bytes_0_3&=0xBFFFFFFF;} 
#define AOLargeHeaderSet_isReserved1(h,e)  {if(e)(h)->large_bytes_0_3|=0x20000000;else(h)->large_bytes_0_3&=0xDFFFFFFF;} 
#define AOLargeHeaderSet_kind(h,e)         {(h)->large_bytes_0_3&=0xE7FFFFFF;(h)->large_bytes_0_3|=(0x18000000&((e)<<27));}
#define AOLargeHeaderSet_applicationSpecific(h,e) \
	                                       {(h)->large_bytes_0_3&=0xF8FFFFFF;(h)->large_bytes_0_3|=(0x07000000&((e)<<24));}
#define AOLargeHeaderSet_rowCount(h,e)     {(h)->large_bytes_0_3&=0xFF0003FF;(h)->large_bytes_0_3|=(0x00FFFC00&((e)<<10));}
#define AOLargeHeaderSet_dataLength(h,e)       {(h)->large_bytes_0_3&=0xFFFFFC00;(h)->large_bytes_0_3|=(0x000003FF&(e>>11));\
                                            (h)->large_bytes_4_7&=0x001FFFFF;(h)->large_bytes_4_7|=(0xFFE00000&((e)<<21));}
#define AOLargeHeaderSet_compressedLength(h,e)  {(h)->large_bytes_4_7&=0xFFE00000;(h)->large_bytes_4_7|=(0x001FFFFF&(e));}

// Assume field is initially zero.
#define AOLargeHeaderInit_Init(h)           {(h)->large_bytes_0_3=0;(h)->large_bytes_4_7=0;} 
#define AOLargeHeaderInit_isLargeheader(h,e) {if(e)(h)->large_bytes_0_3|=0x80000000;} 
#define AOLargeHeaderInit_hasChecksum(h,e)  {if(e)(h)->large_bytes_0_3|=0x40000000;} 
#define AOLargeHeaderInit_isReserved1(h,e)  {if(e)(h)->large_bytes_0_3|=0x20000000;}
#define AOLargeHeaderInit_kind(h,e)         {(h)->large_bytes_0_3|=(0x18000000&((e)<<27));}
#define AOLargeHeaderInit_applicationSpecific(h,e)  \
	                                        {(h)->large_bytes_0_3|=(0x07000000&((e)<<24));}
#define AOLargeHeaderInit_rowCount(h,e)     {(h)->large_bytes_0_3|=(0x00FFFC00&((e)<<10));}

#define AOLargeHeaderInit_dataLength(h,e)   {(h)->large_bytes_0_3|=(0x000003FF&(e>>11));\
                                             (h)->large_bytes_4_7|=(0xFFE00000&((e)<<21));}
#define AOLargeHeaderInit_compressedLength(h,e)  {(h)->large_bytes_4_7|=(0x001FFFFF&(e));}


typedef struct AOSmallHeader
{
/*
 * Can't seem to get this to pack nicely to 32 bits so the header is aligned
 * on an 4 byte boundary, so go to bit shifting...
 */

//  unsigned          isLargeHeader:1;  
//                        /*
//                         * True if this header is the long 64 bit AO
//                         */
//  unsigned          hasChecksum:1;  
//                        /*
//                         * True if there is a 32 bit CRC checksum of the block.
//                         */
//  unsigned          reserved:1;  
//                        /* 
//                         * Reserved for future use.
//                         */
//  unsigned          applicationSpecific:3;  
//                         /* 
//                          * Application (i.e. executor) 3 bit value.  
//                          */
//
//  unsigned          rowCount:10;
//                         /* 
//                          * The number of rows in the block. Maintained in the header so
//                          * blocks can be skipped easily.
//                          */
//
//  unsigned          dataLength:16;
//                         /* Length of the data block.
//                          */

    uint32            bytes_0_3;

} AOSmallHeader;

#define AOSmallHeaderGet_isLargeHeader(h)  ((h)->bytes_0_3>>31)                 // 1 bit
#define AOSmallHeaderGet_hasChecksum(h)    (((h)->bytes_0_3&0x40000000)>>30)    // 1 bit
#define AOSmallHeaderGet_isReserved1(h)    (((h)->bytes_0_3&0x20000000)>>29)    // 1 bit
#define AOSmallHeaderGet_applicationSpecific(h) \
	                                       (((h)->bytes_0_3&0x1C000000)>>26)    // 3 bits
#define AOSmallHeaderGet_rowCount(h)       (((h)->bytes_0_3&0x03FF0000)>>16)    // 10 bits
#define AOSmallHeaderGet_dataLength(h)     ((h)->bytes_0_3&0x0000FFFF)          // 16 bits

// For single bits, set or clear directly.  Otherwise, use AND to clear field then OR to set.
#define AOSmallHeaderSet_isLargeheader(h,e) {if(e)(h)->bytes_0_3|=0x80000000;else(h)->bytes_0_3&=0x7FFFFFFF;} 
#define AOSmallHeaderSet_hasChecksum(h,e)   {if(e)(h)->bytes_0_3|=0x40000000;else(h)->bytes_0_3&=0xBFFFFFFF;} 
#define AOSmallHeaderSet_isReserved1(h,e)   {if(e)(h)->bytes_0_3|=0x20000000;else(h)->bytes_0_3&=0xDFFFFFFF;} 
#define AOSmallHeaderSet_applicationSpecific(h,e) \
	                                        {(h)->bytes_0_3&=0xD3FFFFFF;(h)->bytes_0_3|=(0x1C000000&((e)<<26));}
#define AOSmallHeaderSet_rowCount(h,e)      {(h)->bytes_0_3&=0xFC00FFFF;(h)->bytes_0_3|=(0x04FF0000&((e)<<16));}
#define AOSmallHeaderSet_dataLength(h,e)    {(h)->bytes_0_3&=0xFFFF0000;(h)->bytes_0_3|=(0x0000FFFF&(e));}

// Assume field is initially zero.
#define AOSmallHeaderInit_Init(h)           {(h)->bytes_0_3=0;} 

#define AOSmallHeaderInit_isLargeHeader(h,e) {if(e)(h)->bytes_0_3|=0x80000000;}
#define AOSmallHeaderInit_hasChecksum(h,e)  {if(e)(h)->bytes_0_3|=0x40000000;}
#define AOSmallHeaderInit_isReserved1(h,e)  {if(e)(h)->bytes_0_3|=0x20000000;}
#define AOSmallHeaderInit_applicationSpecific(h,e) \
	                                        {(h)->bytes_0_3|=(0x1C000000&((e)<<26));}
#define AOSmallHeaderInit_rowCount(h,e)     {(h)->bytes_0_3|=(0x04FF0000&((e)<<16));}
#define AOSmallHeaderInit_dataLength(h,e)   {(h)->bytes_0_3|=(0x0000FFFF&(e));}

#endif   /* APPENDONLYSTORAGE_INT_H */

