/*
 * twophase.h
 *
 * Two-phase commit related declarations.
 *
 * $Id$
 */
#ifndef TWOPHASE_H
#define TWOPHASE_H

#include "access/xlog.h"
#include "storage/lock.h"

#define GIDSIZE 200

typedef void (*TwoPhaseCallback) (TransactionId xid, void *recdata, uint32 len);


typedef uint8 TwoPhaseRmgrId;

typedef struct {
	LWLockId ControlLock;
	HTAB *gidHash;

	/*
	 * Dir is set during TwoPhaseShmemInit and does not change thereafter.
	 * The value is automatically inherited by backends via fork, and
	 * doesn't need to be in shared memory.
	 */
	char		Dir[MAXPGPATH];
} TwoPhaseCtlData_s;

extern TwoPhaseCtlData_s TwoPhaseCtlData;

/* I couldn't get sizeof(LockRecord) work for some reason!? */

/* We keep one of these structs for each global transaction
 * that is in prepared state.
 */
typedef struct GXACT {
	char gid[GIDSIZE];
	TransactionId xid;
	AclId owner;
} GXACT;

/* This is used by pg_prepared_xact and it's support function GetPreparedTransactionList
 */
typedef struct PreparedXactArray {
	int nelements;
	GXACT elements[0];
} PreparedXactArray;

#define SizeOfPreparedXactArray(n)	(sizeof(int) + sizeof(GXACT) * (n))


/*
 * Header for a 2PC file
 */
typedef struct TwoPhaseFileHeader
{
	TransactionId xid;
	AclId owner;
	char gid[GIDSIZE];
	int nsubxacts;
} TwoPhaseFileHeader;

extern int max_prepared_xacts;

extern int StartPreparedTransactionListScan(void);
extern TransactionId NextPreparedTransactionId(void);
extern void EndPreparedTransactionListScan(void);


extern int	TwoPhaseShmemSize(void);
extern void TwoPhaseShmemInit(void);
extern void BootStrapTwoPhase(void);

extern void MarkAsBegun(TransactionId xid, char *gid, AclId owner);
extern TransactionId MarkAsNoLongerBegun(char *gid, AclId user);
extern void MarkAsPrepared(TransactionId xid, char *gid, AclId owner);
extern TransactionId MarkAsNoLongerPrepared(char *gid, AclId user);
extern void ListPrepared(void);
extern PreparedXactArray *GetPreparedTransactionList(void);

extern void StartPrepare(char *gid);
extern void EndPrepare(void);

extern void RegisterTwoPhaseRecord(TwoPhaseRmgrId rmid, void *data, uint32 len);
extern void RecoverPreparedTransactions(void);

extern int OpenTwoPhaseFile(TransactionId xid, TwoPhaseFileHeader *hdr, TransactionId **children);
extern void RemoveTwoPhaseFile(TransactionId xid);
extern void RecreateTwoPhaseFile(TransactionId xid, void *content, int len);

extern void ProcessRecords(int fd, TransactionId xid, const TwoPhaseCallback callbacks[]);

extern void CheckPointTwoPhase(void);


#endif   /* TWOPHASE_H */
