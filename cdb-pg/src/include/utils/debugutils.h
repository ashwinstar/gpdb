/*
 * debugutils.h
 * 
 * Debugging tools and routines
 * 
 * Copyright (c) 2007-2008, Greenplum inc
 */

#ifndef _DEBUGUTILS_H_
#define _DEBUGUTILS_H_

#include "executor/tuptable.h"

extern void dotnode(void *, const char*);
extern char *tup2str(TupleTableSlot *slot);
extern void dump_tupdesc(TupleDesc tupdesc, const char *fname);
extern void dump_mt_bind(MemTupleBinding *mt_bind, const char *fname);
#endif /* _DEBUGUTILS_H_ */


