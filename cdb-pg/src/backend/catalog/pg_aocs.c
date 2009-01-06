/*-------------------------------------------------------------------------
*
* pg_aocs.c
*	  routines to support manipulation of the pg_aocs relation
*
* Portions Copyright (c) 2008, Greenplum Inc
* Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
* Portions Copyright (c) 1994, Regents of the University of California
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"

#include "catalog/pg_aocs.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/fmgroids.h"

