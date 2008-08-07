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
// **** Begin Block Header specific 60 bits ****
//

    uint32            header_bytes_0_3;
    uint32            header_bytes_4_7;

} AOHeader;

#define AOHeaderGet_reserved0(h)          ((h)->header_bytes_0_3>>31)                  // 1 bit
#define AOHeaderGet_headerKind(h)         (((h)->header_bytes_0_3&0x70000000)>>28)    // 3 bits

typedef struct AOBlockHeader
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
// **** Begin Block Header specific 60 bits ****
//
//  unsigned          executorBlockKind:4;  
//                         /* 
//                          * Executor 4 bit value.  
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
#define AOBlockHeaderGet_executorBlockKind(h)  (((h)->block_bytes_0_3&0x0F000000)>>24)    // 4 bits
#define AOBlockHeaderGet_rowCount(h)           (((h)->block_bytes_0_3&0x00FFFC00)>>10)    // 14 bits
#define AOBlockHeaderGet_dataLength(h) \
          ((((h)->block_bytes_0_3&0x000003FF)<<11)|(((h)->block_bytes_4_7&0xFFE00000)>>21))
                         // top 10 bits                      lower 11 bits                  21 bits
#define AOBlockHeaderGet_compressedLength(h)   ((h)->block_bytes_4_7&0x001FFFFF)         // 21 bits

// For single bits, set or clear directly.  Otherwise, use AND to clear field then OR to set.
#define AOBlockHeaderSet_reserved0(h,e)         {if(e)(h)->block_bytes_0_3|=0x80000000;else(h)->block_bytes_0_3&=0x7FFFFFFF;} 
#define AOBlockHeaderSet_headerKind(h,e)        {(h)->block_bytes_0_3&=0x8FFFFFFF;(h)->block_bytes_0_3|=(0x70000000&((e)<<28));}
#define AOBlockHeaderSet_executorBlockKind(h,e) {(h)->block_bytes_0_3&=0xF0FFFFFF;(h)->block_bytes_0_3|=(0x0F000000&((e)<<24));}
#define AOBlockHeaderSet_rowCount(h,e)          {(h)->block_bytes_0_3&=0xFF0003FF;(h)->block_bytes_0_3|=(0x00FFFC00&((e)<<10));}
#define AOBlockHeaderSet_dataLength(h,e)        {(h)->block_bytes_0_3&=0xFFFFFC00;(h)->block_bytes_0_3|=(0x000003FF&(e>>11));\
                                                 (h)->block_bytes_4_7&=0x001FFFFF;(h)->block_bytes_4_7|=(0xFFE00000&((e)<<21));}
#define AOBlockHeaderSet_compressedLength(h,e)  {(h)->block_bytes_4_7&=0xFFE00000;(h)->block_bytes_4_7|=(0x001FFFFF&(e));}

// Assume field is initially zero.
#define AOBlockHeaderInit_Init(h)                {(h)->block_bytes_0_3=0;(h)->block_bytes_4_7=0;} 
#define AOBlockHeaderInit_reserved0(h,e)         {if(e)(h)->block_bytes_0_3|=0x80000000;} 
#define AOBlockHeaderInit_headerKind(h,e)        {(h)->block_bytes_0_3|=(0x70000000&((e)<<28));}
#define AOBlockHeaderInit_executorBlockKind(h,e) {(h)->block_bytes_0_3|=(0x0F000000&((e)<<24));}
#define AOBlockHeaderInit_rowCount(h,e)          {(h)->block_bytes_0_3|=(0x00FFFC00&((e)<<10));}

#define AOBlockHeaderInit_dataLength(h,e)        {(h)->block_bytes_0_3|=(0x000003FF&(e>>11));\
                                                  (h)->block_bytes_4_7|=(0xFFE00000&((e)<<21));}
#define AOBlockHeaderInit_compressedLength(h,e)  {(h)->block_bytes_4_7|=(0x001FFFFF&(e));}


#endif   /* APPENDONLYSTORAGE_INT_H */

