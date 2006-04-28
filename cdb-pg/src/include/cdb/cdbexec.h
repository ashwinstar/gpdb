/*-------------------------------------------------------------------------
 *
 * cdbexec.h
 *	  MPPEXEC command
 *
 * Copyright (c) 2004-2006 Greenplum Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBEXEC_H
#define CDBEXEC_H

struct CdbExecStmt;                     /* #include "nodes/parsenodes.h" */
struct _DestReceiver;                   /* #include "tcop/dest.h" */


/* Utility statement MPPEXEC */
void CdbExecQuery(struct CdbExecStmt * stmt, struct _DestReceiver *dest, char *completionTag);

#endif   /* CDBEXEC_H */
