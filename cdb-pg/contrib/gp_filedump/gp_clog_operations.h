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

/*Externally callable functions*/
extern void gp_clog_print_all_commit_status(FILE *);
extern void gp_clog_validate_clog_filename(char *);

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
#define CLOG_PAGES_PER_SEG  32

#define TransactionIdToBIndex(axid) (axid % CLOG_XACTS_PER_BYTE)

#endif /* GP_CLOG_OPERATIONS_H_ */
