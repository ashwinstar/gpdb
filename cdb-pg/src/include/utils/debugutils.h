/*
 * debugutils.h
 * 
 * Debugging tools and routines
 * 
 * Copyright (c) 2007, Greenplum
 */

#ifndef _DEBUGUTILS_H_
#define _DEBUGUTILS_H_

#include "executor/tuptable.h"

extern void dotnode(void *, const char*);
extern char *tup2str(TupleTableSlot *slot);
#endif /* _DEBUGUTILS_H_ */


