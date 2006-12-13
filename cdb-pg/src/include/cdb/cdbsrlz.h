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

extern char *serializeNode(Node *node, int *size);
extern Node *deserializeNode(const char *strNode, int size);

/* this is useful for debugging */
extern char * uncompress_string(const char *src, int size);

#endif   /* CDBSRLZ_H */
