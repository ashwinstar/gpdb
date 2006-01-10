/*-------------------------------------------------------------------------
 *
 * cdbsrlz.h
 *	  definitions for paln serialization utilities
 *
 * Copyright (c) 2004, Greenplum
 *
 * NOTES
 *
 *-------------------------------------------------------------------------
 */

#ifndef CDBSRLZ_H
#define CDBSRLZ_H

#include "lib/stringinfo.h"
#include "nodes/pg_list.h"

/*
 * The following code was developed to test xml serialization.
 * We can delete it all later if it turns out we don't need it.
 * For now, we'll just get rid of it with an #if 0
 */
#if 0
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

typedef struct TagStack
{
	int			capacity;
	int			size;
	char	  **tags;
}	TagStack;

typedef struct SerializeContext
{
	bool		fromNode;

	TagStack   *tagStack;

	/*
	 * Used when fromNode == true, i.e. during serialization
	 */
	StringInfoData result;

	/*
	 * Used when fromNode == false, i.e. during deserialization
	 */
	char	   *input;
	int			offset;
	bool		eof;
}	SerializeContext;


extern void serializePlan(Plan *plan, SerializeContext * context);
extern bool deSerializePlan(Plan **pplan, SerializeContext * context, char *szInput);
extern void destroySerializeContext(SerializeContext * context);
#endif

extern char *serializeNode(Node *node, int *size);
extern Node *deserializeNode(char *strNode);

#endif   /* CDBSRLZ_H */
