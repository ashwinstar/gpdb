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

static char *unescape_quotes(const char *src);
static char *escape_quotes(const char *src, int *size);

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

	pszNode = nodeToStringFast(node);
	Assert(pszNode != NULL);

	sNode = escape_quotes(pszNode, size);
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
deserializeNode(char *strNode)
{
	char		*sNode;
	Node		*node;

	Assert(strNode != NULL);

	sNode = unescape_quotes(strNode);

	Assert(sNode != NULL);

	node = stringToNode(sNode);

	pfree(sNode);

	return node;
}


/*
 * Escape any single quotes in given string
 * 
 * If size argument is non-NULL, store the size (not counting terminating NULL).
 */
static char *
escape_quotes(const char *src, int *size)
{
	int			len = strlen(src),
				i,
				j;
	char	   *result = palloc(len * 2 + 1);

	for (i = 0, j = 0; i < len; i++)
	{
		if (src[i] == '\'' || src[i] == '\\')
			result[j++] = '\\';
		result[j++] = src[i];
	}

	if (size != NULL)
		*size = j;

	result[j] = '\0';
	return result;
}

/*
 * Unescape any escaped single quotes in given string
 */
static char *
unescape_quotes(const char *src)
{
	int			len = strlen(src),
				i,
				j;
	char	   *result = palloc(len + 1);

	for (i = 0, j = 0; i < len; i++)
	{
		if (i < len - 1 && src[i] == '\\' && (src[i + 1] == '\'' || src[i + 1] == '\\'))
			i++;

		result[j++] = src[i];
	}
	result[j] = '\0';
	return result;
}
