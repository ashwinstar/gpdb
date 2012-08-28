/*
 * gp_clog_operations.h
 */

#ifndef _GP_CLOG_OPERATIONS_H_
#define _GP_CLOG_OPERATIONS_H_

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <postgres.h>
#include <math.h>
#include <assert.h>
#include <ctype.h>
#include <unistd.h>

#include "storage/bufpage.h"
#include "pg_config_manual.h"
#include "gp_clog_operations.h"
#include "storage/bufpage.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/gin.h"
#include "access/gist.h"
#include "access/nbtree.h"
#include "access/itup.h"
#include "access/htup.h"
#include "catalog/pg_control.h"
#include "utils/builtins.h"

extern void gp_clog_print_ret_status(int retCode);
extern int  gp_clog_print_xid_commit_status(FILE *targetFile, unsigned int xid);
extern int  gp_clog_update_xid_commit_status(FILE *clogFile, unsigned int xid, int statusToBeSet);
extern int  gp_clog_validate_clog_filename(char *clogFileName, unsigned int *lowBoundXid, unsigned int *highBoundXid);

/*Return codes*/
#define RETOK 0
#define RETERRFILEOF 1
#define RETERRFILEOFRDERROR 2
#define RETERRINVALIDIP 3
#define RETERRTOOLONG 4
#define RETERROUTOFRANGE 5
#define RETWARNINVALIDFILENAME 6

/*Read operations*/
#define TRANSACTION_STATUS_IN_PROGRESS          0x00
#define TRANSACTION_STATUS_COMMITTED            0x01
#define TRANSACTION_STATUS_ABORTED              0x02
#define TRANSACTION_STATUS_SUB_COMMITTED        0x03

/*Refer Clog.c and Slru.c (under main GPDB code) for more details*/
#define CLOG_BITS_PER_XACT  2
#define CLOG_XACTS_PER_BYTE 4
#define CLOG_XACTS_PER_PAGE (BLCKSZ * CLOG_XACTS_PER_BYTE)
#define CLOG_XACT_BITMASK   ((1 << CLOG_BITS_PER_XACT) - 1)
#define CLOG_PAGES_PER_SEG 32

#define TransactionIdToBIndex(axid) (axid % CLOG_XACTS_PER_BYTE)

#endif /* _GP_CLOG_OPERATIONS_H_ */
