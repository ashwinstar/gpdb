#ifndef QLZ1_HEADER
#define QLZ1_HEADER

// Copyright (C) 2006-2008 Lasse Mikkel Reinhold
// lar@quicklz.com
//
// QuickLZ can be used for free under the GPL-1 or GPL-2 license (where anything 
// released into public must be open source) or under a commercial license if such 
// has been acquired (see http://www.quicklz.com/order.html). The commercial license 
// does not cover derived or ported versions created by third parties under GPL.

// You can edit following user settings. Note that data must be decompressed with the
// same setting of QLZ1_COMPRESSION_LEVEL and QLZ1_STREAMING_BUFFER as it was compressed
// (see the manual). First #ifndef makes it possible to define settings from the outside 
// like the compiler command line or from higher level code.

/*
 * GPDB NOTE: the following header includes settings and operations for quicklz
 * compression level 1 only. See README_QUICKLZ for more information.
 */

#ifndef QLZ1_COMPRESSION_LEVEL
	#define QLZ1_COMPRESSION_LEVEL 1
	#define QLZ1_STREAMING_BUFFER 0
#endif

// Version 1.4.0 final (negative revision means beta)
#define QLZ1_VERSION_MAJOR 1
#define QLZ1_VERSION_MINOR 4
#define QLZ1_VERSION_REVISION 0

// Using size_t, memset() and memcpy()
#include <string.h>

// Public functions of QuickLZ
size_t qlz1_size_decompressed(const char *source);
size_t qlz1_size_compressed(const char *source);
size_t qlz1_decompress(const char *source, void *destination, char *scratch_decompress);
size_t qlz1_compress(const void *source, char *destination, size_t size, char *scratch_compress);
int qlz1_get_setting(int setting);

// Verify compression level
#if QLZ1_COMPRESSION_LEVEL != 1
ereport(ERROR,
		(errcode(ERRCODE_GP_INTERNAL_ERROR),
		 errmsg("QLZ1_COMPRESSION_LEVEL must be 1")));
#endif

// Compute QLZ1_SCRATCH_COMPRESS and QLZ1_SCRATCH_DECOMPRESS
#define QLZ1_POINTERS 1
#define QLZ1_HASH_VALUES 4096

typedef struct 
{
	unsigned int cache[QLZ1_POINTERS];
	const unsigned char *offset[QLZ1_POINTERS];
} qlz1_hash_compress;

typedef struct 
{
	const unsigned char *offset[QLZ1_POINTERS];
} qlz1_hash_decompress;


#define QLZ1_ALIGNMENT_PADD 8
#define QLZ1_BUFFER_COUNTER 8

#define QLZ1_SCRATCH_COMPRESS QLZ1_ALIGNMENT_PADD + QLZ1_BUFFER_COUNTER + QLZ1_STREAMING_BUFFER + sizeof(qlz1_hash_compress[QLZ1_HASH_VALUES]) + QLZ1_HASH_VALUES
#define QLZ1_SCRATCH_DECOMPRESS QLZ1_ALIGNMENT_PADD + QLZ1_BUFFER_COUNTER + QLZ1_STREAMING_BUFFER + sizeof(qlz1_hash_decompress[QLZ1_HASH_VALUES]) + QLZ1_HASH_VALUES

#endif





