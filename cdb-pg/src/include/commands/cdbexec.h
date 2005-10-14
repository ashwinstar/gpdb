/*-------------------------------------------------------------------------
 *
 * cdbexec.h
 *	  MPPEXEC command
 *
 *
 * Copyright (c) 2002-2004, PostgreSQL Global Development Group
 * Copyright (c) 2005 Greenplum Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBEXEC_H
#define CDBEXEC_H

#include "executor/executor.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"


/* Utility statement MPPEXEC */
extern void CdbExecQuery(CdbExecStmt * stmt, DestReceiver *dest, char *completionTag);

#endif   /* CDBEXEC_H */
