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

#include "lib/stringinfo.h"

/* global function declarations */
int cdbCopyGetDbCount(int total_segs, int seg);
int cdbCopyStart(int total_segs, int seg, int db, char *copyCmd);
int cdbCopySendData(int total_segs, int seg, int db, const char *buffer, int nbytes);
int cdbCopyEnd(int total_segs, int seg, int db, StringInfoData errMessage);
int cdbCopyExecTrax(int total_segs, int seg, int db, char *sql);

#endif   /* CDBCOPY_H */
