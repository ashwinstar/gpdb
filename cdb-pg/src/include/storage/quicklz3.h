#ifndef QLZ3_HEADER
#define QLZ3_HEADER

// Copyright (C) 2006-2008 Lasse Mikkel Reinhold
// lar@quicklz.com
//
// QuickLZ can be used for free under the GPL-1 or GPL-2 license (where anything 
// released into public must be open source) or under a commercial license if such 
// has been acquired (see http://www.quicklz.com/order.html). The commercial license 
// does not cover derived or ported versions created by third parties under GPL.

// You can edit following user settings. Note that data must be decompressed with the
// same setting of QLZ3_COMPRESSION_LEVEL and QLZ3_STREAMING_BUFFER as it was compressed
// (see the manual). First #ifndef makes it possible to define settings from the outside 
// like the compiler command line or from higher level code.

/*
 * GPDB NOTE: the following header includes settings and operations for quicklz
 * compression level 3 only. See README_QUICKLZ for more information.
 */

#ifndef QLZ3_COMPRESSION_LEVEL
	#define QLZ3_COMPRESSION_LEVEL 3
	#define QLZ3_STREAMING_BUFFER 0
#endif

// Version 1.4.0 final (negative revision means beta)
#define QLZ3_VERSION_MAJOR 1
#define QLZ3_VERSION_MINOR 4
#define QLZ3_VERSION_REVISION 0

// Using size_t, memset() and memcpy()
#include <string.h>

// Public functions of QuickLZ
size_t qlz3_size_decompressed(const char *source);
size_t qlz3_size_compressed(const char *source);
size_t qlz3_decompress(const char *source, void *destination, char *scratch_decompress);
size_t qlz3_compress(const void *source, char *destination, size_t size, char *scratch_compress);
int qlz3_get_setting(int setting);

// Verify compression level
#if QLZ3_COMPRESSION_LEVEL != 3
ereport(ERROR,
		(errcode(ERRCODE_GP_INTERNAL_ERROR),
		 errmsg("QLZ3_COMPRESSION_LEVEL must be 3")));
#endif

// Compute QLZ3_SCRATCH_COMPRESS and QLZ3_SCRATCH_DECOMPRESS
#define QLZ3_POINTERS 16
#define QLZ3_HASH_VALUES 4096

typedef struct 
{
	const unsigned char *offset[QLZ3_POINTERS];
} qlz3_hash_compress;

typedef struct 
{
	const unsigned char *offset[QLZ3_POINTERS];
} qlz3_hash_decompress;


#define QLZ3_ALIGNMENT_PADD 8
#define QLZ3_BUFFER_COUNTER 8

#define QLZ3_SCRATCH_COMPRESS QLZ3_ALIGNMENT_PADD + QLZ3_BUFFER_COUNTER + QLZ3_STREAMING_BUFFER + sizeof(qlz3_hash_compress[QLZ3_HASH_VALUES]) + QLZ3_HASH_VALUES
#define QLZ3_SCRATCH_DECOMPRESS QLZ3_ALIGNMENT_PADD + QLZ3_BUFFER_COUNTER + QLZ3_STREAMING_BUFFER

#endif





