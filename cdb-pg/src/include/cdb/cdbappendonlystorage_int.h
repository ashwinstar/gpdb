/*-------------------------------------------------------------------------
 *
 * cdbappendonlystorage.h
 *
 * Copyright (c) 2007-2009, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBAPPENDONLYSTORAGE_INT_H
#define CDBAPPENDONLYSTORAGE_INT_H

/*
 * For checksum protection that doesn’t require examining the unchecked data,
 * we always use a 64 bit header.  
 *
 * So, 64 bit header + [ block checksum + header checksum ].  When the table
 * global flag indicates checksumming, there will be 2 32-bit checksums.  The
 * header checksum protects the header and the block checksum.  If it is
 * valid, the header fields can be used.
 *
 * The first 4 bits of the header are 1 bit reserved and 3 bits header kind.
 * The remaining 60 bits are header kind specific.
 */

/*
 * You can think of this struct as the base class for AOBlockHeader, etc.
 */

typedef struct AOHeader
{
/*
 * Can't seem to get this to pack nicely to 64 bits so the header is aligned
 * on an 8 byte boundary, so go to bit shifting...
 */

//  unsigned          reserved0:1;  
//                        /*
//                         * Reserved for future use.
//                         */
//  AOHeaderKind 	  headerKind:3;  
//                        /* 
//                         * Which kind of header is it?
//                         */
//
// **** Begin Header kind specific 60 bits ****
//

    uint32            header_bytes_0_3;
    uint32            header_bytes_4_7;

} AOHeader;

#define AOHeaderGet_reserved0(h)          ((h)->header_bytes_0_3>>31)                  // 1 bit
#define AOHeaderGet_headerKind(h)         (((h)->header_bytes_0_3&0x70000000)>>28)    // 3 bit

typedef struct AOBlockHeader
{
/*
 * Can't seem to get this to pack nicely to 64 bits so the header is aligned
 * on an 8 byte boundary, so go to bit shifting...
 */

//
// **** These fields are common to all Append-Only Storage Header kinds.  ****
// **** See AOHeader. ****
//
//  unsigned          reserved0:1;  
//                        /*
//                         * Reserved for future use.
//                         */
//  AOHeaderKind 	  headerKind:3;  
//                        /* 
//                         * Which kind of header is it?
//                         */
//
// **** Begin Block Header specific 60 bits ****
//
// (NOTE: Previously, the executorBlockKind was 4 bits.  We stole one bit for the
//           hasFirstRowNum flag.  Use of that flag has to be versioned above)
//
//  unsigned          hasFirstRowNum:1;
//				  /*
//                          * When true, the first row number is stored as extra header
//                          * information.
//                          */
//
//  unsigned          executorBlockKind:3;  
//                         /* 
//                          * Executor 3 bit value.  
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

    uint32            block_bytes_0_3;
    uint32            block_bytes_4_7;

} AOBlockHeader;

#define AOBlockHeaderGet_reserved0(h)          ((h)->block_bytes_0_3>>31)                  // 1 bit
#define AOBlockHeaderGet_headerKind(h)         (((h)->block_bytes_0_3&0x70000000)>>28)    // 3 bits
#define AOBlockHeaderGet_hasFirstRowNum(h)     (((h)->block_bytes_0_3&0x08000000)>>27)    // 1 bits
#define AOBlockHeaderGet_executorBlockKind(h)  (((h)->block_bytes_0_3&0x07000000)>>24)    // 3 bits
#define AOBlockHeaderGet_rowCount(h)           (((h)->block_bytes_0_3&0x00FFFC00)>>10)    // 14 bits
#define AOBlockHeaderGet_dataLength(h) \
          ((((h)->block_bytes_0_3&0x000003FF)<<11)|(((h)->block_bytes_4_7&0xFFE00000)>>21))
                         // top 10 bits                      lower 11 bits                  21 bits
#define AOBlockHeaderGet_compressedLength(h)   ((h)->block_bytes_4_7&0x001FFFFF)         // 21 bits

// For single bits, set or clear directly.  Otherwise, use AND to clear field then OR to set.
#define AOBlockHeaderSet_reserved0(h,e)         {if(e)(h)->block_bytes_0_3|=0x80000000;else(h)->block_bytes_0_3&=0x7FFFFFFF;} 
#define AOBlockHeaderSet_headerKind(h,e)        {(h)->block_bytes_0_3&=0x8FFFFFFF;(h)->block_bytes_0_3|=(0x70000000&((e)<<28));}
#define AOBlockHeaderSet_hasFirstRowNum(h,e)    {if(e)(h)->block_bytes_0_3|=0x08000000;else(h)->block_bytes_0_3&=0xF7FFFFFF;}
#define AOBlockHeaderSet_executorBlockKind(h,e) {(h)->block_bytes_0_3&=0xF8FFFFFF;(h)->block_bytes_0_3|=(0x07000000&((e)<<24));}
#define AOBlockHeaderSet_rowCount(h,e)          {(h)->block_bytes_0_3&=0xFF0003FF;(h)->block_bytes_0_3|=(0x00FFFC00&((e)<<10));}
#define AOBlockHeaderSet_dataLength(h,e)        {(h)->block_bytes_0_3&=0xFFFFFC00;(h)->block_bytes_0_3|=(0x000003FF&(e>>11));\
                                                 (h)->block_bytes_4_7&=0x001FFFFF;(h)->block_bytes_4_7|=(0xFFE00000&((e)<<21));}
#define AOBlockHeaderSet_compressedLength(h,e)  {(h)->block_bytes_4_7&=0xFFE00000;(h)->block_bytes_4_7|=(0x001FFFFF&(e));}

// Assume field is initially zero.
#define AOBlockHeaderInit_Init(h)                {(h)->block_bytes_0_3=0;(h)->block_bytes_4_7=0;} 
#define AOBlockHeaderInit_reserved0(h,e)         {if(e)(h)->block_bytes_0_3|=0x80000000;} 
#define AOBlockHeaderInit_headerKind(h,e)        {(h)->block_bytes_0_3|=(0x70000000&((e)<<28));}
#define AOBlockHeaderInit_hasFirstRowNum(h,e)    {if(e)(h)->block_bytes_0_3|=0x08000000;} 
#define AOBlockHeaderInit_executorBlockKind(h,e) {(h)->block_bytes_0_3|=(0x07000000&((e)<<24));}
#define AOBlockHeaderInit_rowCount(h,e)          {(h)->block_bytes_0_3|=(0x00FFFC00&((e)<<10));}

#define AOBlockHeaderInit_dataLength(h,e)        {(h)->block_bytes_0_3|=(0x000003FF&(e>>11));\
                                                  (h)->block_bytes_4_7|=(0xFFE00000&((e)<<21));}
#define AOBlockHeaderInit_compressedLength(h,e)  {(h)->block_bytes_4_7|=(0x001FFFFF&(e));}


typedef struct AOLargeContentHeader
{

// **** These fields are common to all Append-Only Storage Header kinds.  ****
// **** See AOHeader. ****
//
//  unsigned          reserved0:1;  
//                        /*
//                         * Reserved for future use.
//                         */
//  AOHeaderKind 	  headerKind:3;  
//                        /* 
//                         * Which kind of header is it?
//                         */
//
// **** Begin Large Content Info Header specific 60 bits ****
//
//  unsigned          hasFirstRowNum:1;
//				  /*
//                          * When true, the first row number is stored as extra header
//                          * information.
//                          */
//
//  unsigned          executorBlockKind:3;  
//                         /* 
//                          * Executor 3 bit value.  
//                          */
//
//
//  unsigned          reserved1:1;
//                        /*
//                         * Reserved for future use.
//                         */
//
//  unsigned          largeRowCount:25;
//                         /* 
//                          * The number of rows in the large content. Maintained in the header so
//                          * blocks can be skipped without decompression, etc.
//                          */
//
//  unsigned          largeContentLength:30;
//                         /* 
//                          * Total byte length of the large content, from 1 to 1Gb - 1 bytes.
//                          */

    uint32            largecontent_bytes_0_3;
    uint32            largecontent_bytes_4_7;

} AOLargeContentHeader;

#define AOLargeContentHeaderGet_reserved0(h)          ((h)->largecontent_bytes_0_3>>31)                  // 1 bit
#define AOLargeContentHeaderGet_headerKind(h)         (((h)->largecontent_bytes_0_3&0x70000000)>>28)    // 3 bits
#define AOLargeContentHeaderGet_hasFirstRowNum(h)     (((h)->largecontent_bytes_0_3&0x08000000)>>27)    // 1 bits
#define AOLargeContentHeaderGet_executorBlockKind(h)  (((h)->largecontent_bytes_0_3&0x07000000)>>24)    // 3 bits
#define AOLargeContentHeaderGet_reserved1(h)          (((h)->largecontent_bytes_0_3&0x00800000)>>23)    // 1 bit
#define AOLargeContentHeaderGet_largeRowCount(h) \
          ((((h)->largecontent_bytes_0_3&0x007FFFFF)<<2)|(((h)->largecontent_bytes_4_7&0xC0000000)>>30))
                         // top 23 bits                                                             lower 2 bits                                      25 bits
#define AOLargeContentHeaderGet_largeContentLength(h)   ((h)->largecontent_bytes_4_7&0x3FFFFFFF)         // 30 bits

// For single bits, set or clear directly.  Otherwise, use AND to clear field then OR to set.
#define AOLargeContentHeaderSet_reserved0(h,e)         {if(e)(h)->largecontent_bytes_0_3|=0x80000000;else(h)->largecontent_bytes_0_3&=0x7FFFFFFF;} 
#define AOLargeContentHeaderSet_headerKind(h,e)        {(h)->largecontent_bytes_0_3&=0x8FFFFFFF;(h)->largecontent_bytes_0_3|=(0x70000000&((e)<<28));}
#define AOLargeContentHeaderSet_hasFirstRowNum(h,e)    {if(e)(h)->largecontent_bytes_0_3|=0x08000000;else(h)->largecontent_bytes_0_3&=0xF7FFFFFF;}
#define AOLargeContentHeaderSet_executorBlockKind(h,e) {(h)->largecontent_bytes_0_3&=0xF8FFFFFF;(h)->largecontent_bytes_0_3|=(0x07000000&((e)<<24));}
#define AOLargeContentHeaderSet_reserved1(h,e)         {if(e)(h)->largecontent_bytes_0_3|=0x00800000;else(h)->largecontent_bytes_0_3&=0xFF7FFFFF;} 
#define AOLargeContentHeaderSet_largeRowCount(h,e)     {(h)->largecontent_bytes_0_3&=0xFF800000;(h)->largecontent_bytes_0_3|=(0x007FFFFF&(e>>2));\
                                                       (h)->largecontent_bytes_4_7&=0x3FFFFFFF;(h)->largecontent_bytes_4_7|=(0xC0000000&((e)<<30));}
#define AOLargeContentHeaderSet_largeContentLength(h,e){(h)->largecontent_bytes_4_7&=0xC0000000;(h)->largecontent_bytes_4_7|=(0x3FFFFFFF&(e));}

// Assume field is initially zero.
#define AOLargeContentHeaderInit_Init(h)                {(h)->largecontent_bytes_0_3=0;(h)->largecontent_bytes_4_7=0;} 
#define AOLargeContentHeaderInit_reserved0(h,e)         {if(e)(h)->largecontent_bytes_0_3|=0x80000000;} 
#define AOLargeContentHeaderInit_headerKind(h,e)        {(h)->largecontent_bytes_0_3|=(0x70000000&((e)<<28));}
#define AOLargeContentHeaderInit_hasFirstRowNum(h,e)    {if(e)(h)->largecontent_bytes_0_3|=0x08000000;} 
#define AOLargeContentHeaderInit_executorBlockKind(h,e) {(h)->largecontent_bytes_0_3|=(0x07000000&((e)<<24));}
#define AOLargeContentHeaderInit_reserved1(h,e)         {if(e)(h)->largecontent_bytes_0_3|=0x00800000;} 
#define AOLargeContentHeaderInit_largeRowCount(h,e)     {(h)->largecontent_bytes_0_3|=(0x007FFFFF&(e>>2));\
                                                        (h)->largecontent_bytes_4_7|=(0xC0000000&((e)<<30));}
#define AOLargeContentHeaderInit_largeContentLength(h,e){(h)->largecontent_bytes_4_7|=(0x3FFFFFFF&(e));}

#endif   /* CDBAPPENDONLYSTORAGE_INT_H */



