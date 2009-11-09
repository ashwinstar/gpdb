/*-------------------------------------------------------------------------
 *
 * cdbllize.h
 *	  definitions for cdbmutate.c utilities
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *
 * IDENTIFICATION
 *	         $Id: //cdb2/main/cdb-pg/src/include/cdb/cdbmutate.h#41 $
 *       $Change: 29753 $
 *     $DateTime: 2008/11/17 11:31:51 $
 *       $Author: ftian $
 *-------------------------------------------------------------------------
 */
#ifndef CDBTARGETEDDISPATCH_H
#define CDBTARGETEDDISPATCH_H

#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"

extern void AssignContentIdsToPlanData(Query *query, Plan *plan);

#endif   /* CDBTARGETEDDISPATCH_H */
