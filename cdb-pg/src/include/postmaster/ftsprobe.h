/*-------------------------------------------------------------------------
 *
 * seqserver.c
 *	  Process under QD postmaster used for doling out sequence values to
 * 		QEs.
 *
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 * $Id: //cdb2/private/tkordas/fts_cleanup/cdb-pg/src/include/postmaster/seqserver.h#1 $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FTSPROVE_H
#define FTSPROBE_H

extern int ftsprobe_start(void);

extern char *find_superuser_name(bool try_bootstrap);

extern bool FtsMasterShutdownRequested(void);
extern void FtsRequestMasterShutdown(void);

#endif   /* FTSPROBE_H */
