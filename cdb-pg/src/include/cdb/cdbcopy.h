/*--------------------------------------------------------------------------
*
* cdbcopy.h
*    Definitions and API functions for cdbcopy.c 
*    These are functions that are used by the backend
*    COPY command in CDB.
*
* Copyright (c) 2005, Metapa
*
* IDENTIFICATION
*    $Id$
*
*--------------------------------------------------------------------------
*/
#ifndef CDBCOPY_H
#define CDBCOPY_H

/* global function declarations */
int cdbCopyGetNumSegs(void);
int cdbCopyGetDbCount(int seg_num);
int cdbCopyStart(int seg, int db, char *copyCmd);
int cdbCopySendData(int seg, int db, const char *buffer, int nbytes);
int cdbCopyEnd(int seg, int db, char *errMessage);
int cdbCopyExecTrax(int seg, int db, char *sql);

#endif   /* CDBCOPY_H */