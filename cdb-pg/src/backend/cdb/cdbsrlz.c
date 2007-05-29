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
#include "regex/regex.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "optimizer/clauses.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbsrlz.h"
#include "nodes/print.h"
#include <math.h>
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif


static char *compress_string(const char *src, int uncompressed_size, int *size);
static char *uncompress_string(const char *src, int size, int * uncompressed_len );

/*
 * compressBound doesn't exist in older zlibs, so let's use our own
 */
static
unsigned long 
gp_compressBound(unsigned long sourceLen)
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
	int		   uncompressed_size;

	Assert(node != NULL);
	Assert(size != NULL);

	pszNode = nodeToBinaryStringFast(node, &uncompressed_size);
	Assert(pszNode != NULL);

	sNode = compress_string(pszNode, uncompressed_size, size);
	pfree(pszNode);
	
	if (DEBUG5 >= log_min_messages)
	{
		Node * newnode = NULL;
		PG_TRY();
		{
		 	newnode = deserializeNode(sNode, *size);
		}
		PG_CATCH();
		{
			elog_node_display(NOTICE, "Before serialization", node, true);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* Some plans guarantee these differences (see serialization
		 * of plan nodes -- they avoid sending QD-only info out) */
		if (strcmp(nodeToString(node), nodeToString(newnode)) != 0)
		{
			elog_node_display(NOTICE, "Before serialization", node, true);

			elog_node_display(NOTICE, "After deserialization", newnode, true);
		}
	}

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
	int 		uncompressed_len;

	Assert(strNode != NULL);

	sNode = uncompress_string(strNode, size, &uncompressed_len);

	Assert(sNode != NULL);
	
	node = readNodeFromBinaryString(sNode, uncompressed_len);

	pfree(sNode);

	return node;
}


/*
 * Compress a (binary) string using zlib.
 * 
 * returns the compressed data and the size of the compressed data.
 */
static char *
compress_string(const char *src, int uncompressed_size, int *size)
{
	int level = 3;
	unsigned long compressed_size;

	Bytef * result;

	Assert(size!=NULL);
	
	if (src == NULL)
	{
		*size = 0;
		return NULL;
	}
	
	compressed_size = gp_compressBound(uncompressed_size);  /* worst case */
	
	result = palloc(compressed_size + sizeof(int));
	memcpy(result,&uncompressed_size,sizeof(int)); 		/* save the original length */
	
	if (compress2(result+4, &compressed_size, (Bytef *)src, uncompressed_size, level) != Z_OK)
		elog(ERROR,"Compression failed");
		
	*size = compressed_size + sizeof(int);
	elog(DEBUG1,"Compressed from %d to %d ",(int)uncompressed_size,(int)*size);

	return (char *)result;
}

/*
 * Uncompress the binary string 
 */
static char *
uncompress_string(const char *src, int size, int * uncompressed_len )
{
	Bytef * result;
	unsigned long resultlen;
	*uncompressed_len = 0;
	
	if (src==NULL)
		return NULL;
		
	Assert(size >= sizeof(int));
		
	memcpy(uncompressed_len,src,sizeof(int));
	
	resultlen = *uncompressed_len;
	result = palloc(resultlen);
		
	if (uncompress(result, &resultlen, (Bytef *)(src+sizeof(int)), size-sizeof(int)) != Z_OK)
		elog(ERROR,"Uncompress failed");
		
	return (char *)result;
}
