/*
 * cdbdistributedxidmap.h
 *		Maps distributed DTM XIDs to local xids 
 *
 * Copyright (c) 2007, greenplum inc
 */
#ifndef DISTRIBUTEDXIDMAP_H
#define DISTRIBUTEDXIDMAP_H

/* Number of SLRU buffers to use for subtrans */
#define NUM_DISTRIBUTEDXIDMAP_BUFFERS	32

extern void AllocOrGetLocalXidForDistributedTransaction(DistributedTransactionId gxid, TransactionId *xid);
extern void AddLocalXidsForDistributedTransactions(DistributedTransactionId *gxidArray, uint32 count);
extern TransactionId GetLocalXidForDistributedTransaction(DistributedTransactionId gxid);

extern Size DistributedXidMapShmemSize(void);
extern void DistributedXidMapShmemInit(void);
extern void BootStrapDistributedXidMap(void);
extern void StartupDistributedXidMap(void);
extern void ShutdownDistributedXidMap(void);
extern void CheckPointDistributedXidMap(void);
extern void TruncateDistributedXidMap(DistributedTransactionId oldestXact);

#endif   /* DISTRIBUTEDXIDMAP_H */

