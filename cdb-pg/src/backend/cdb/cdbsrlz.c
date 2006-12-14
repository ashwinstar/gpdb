/*-------------------------------------------------------------------------
 *
 * cdbsrlz.c
 *	  Serialize a PostgreSQL sequential plan tree.
 *
 * Copyright (c) 2004, Greenplum
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <regex.h>
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbsrlz.h"
#include <math.h>
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif


static char *compress_string(const char *src, int *size);

/*
 * compressBound doesn't exist in older zlibs, so let's use our own
 */
static
unsigned long 
mpp_compressBound(unsigned long sourceLen)
{
  return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + 11;
}

/*
 * serializeNode -
 * This is used on the query dispatcher to serialize Plan and Query Trees for
 * dispatching to qExecs.
 * The returned string is palloc'ed in the current memory context.
 */
char *
serializeNode(Node *node, int *size)
{
	char	   *pszNode;
	char	   *sNode;

	Assert(node != NULL);
	Assert(size != NULL);

	pszNode = nodeToStringFast(node);
	Assert(pszNode != NULL);

	sNode = compress_string(pszNode, size);
	pfree(pszNode);

	return sNode;
}

/*
 * deserializeNode -
 * This is used on the qExecs to deserialize serialized Plan and Query Trees
 * received from the dispatcher.
 * The returned node is palloc'ed in the current memory context.
 */
Node *
deserializeNode(const char *strNode, int size)
{
	char		*sNode;
	Node		*node;

	Assert(strNode != NULL);

	sNode = uncompress_string(strNode, size);

	Assert(sNode != NULL);

	node = stringToNode(sNode);

	pfree(sNode);

	return node;
}


/*
 * Compress a string using zlib.
 * 
 * returns the compressed data and the size of the compressed data.
 */
static char *
compress_string(const char *src, int *size)
{
	int level = 3;
	unsigned long compressed_size;
	int uncompressed_size;
	Bytef * result;

	Assert(size!=NULL);
	
	if (src == NULL)
	{
		*size = 0;
		return NULL;
	}
	
	uncompressed_size = (int)strlen(src)+1;  /* include the trailing zero byte */
	compressed_size = mpp_compressBound(uncompressed_size);  /* worst case */
	
	result = palloc(compressed_size + sizeof(int));
	memcpy(result,&uncompressed_size,sizeof(int)); 		/* save the original length */
	
	if (compress2(result+4, &compressed_size, (Bytef *)src, strlen(src)+1, level) != Z_OK)
		elog(ERROR,"Compression failed");
		
	*size = compressed_size + sizeof(int);
	elog(DEBUG1,"Compressed from %d to %d ",(int)strlen(src),(int)*size);

	return (char *)result;
}

/*
 * Uncompress the string 
 */
char *
uncompress_string(const char *src, int size)
{
	Bytef * result;
	unsigned long resultlen;
	int uncompressed_len = 0;
	
	if (src==NULL)
		return NULL;
		
	Assert(size >= sizeof(int));
		
	memcpy(&uncompressed_len,src,sizeof(int));
	
	resultlen = uncompressed_len;
	result = palloc(resultlen);
		
	if (uncompress(result, &resultlen, (Bytef *)(src+sizeof(int)), size-sizeof(int)) != Z_OK)
		elog(ERROR,"Uncompress failed");
		
	return (char *)result;
}
