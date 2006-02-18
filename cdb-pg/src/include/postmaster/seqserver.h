/*-------------------------------------------------------------------------
 *
 * seqserver.c
 *	  Process under QD postmaster used for doling out sequence values to
 * 		QEs.
 *
 *
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef SEQSERVER_H
#define SEQSERVER_H



/* TODO: eventually we may want different requests to the sequence server
 * 	     other than just next_val().  at that point we should generalize
 * 		 the request/response processing code here and in the seqserver
 */

typedef struct NextValRequest
{

	Oid 	dbid;
	Oid		tablespaceid;	
	Oid     seq_oid;
	bool	isTemp;
	int16   qd_pid;
}	NextValRequest;


typedef struct NextValResponse
{
	int64  	 plast;
	int64    pcached;
	int64    pincrement;
} NextValResponse;



extern void SeqServerShmemInit(void);
extern int SeqServerShmemSize(void);
extern int	seqserver_start(void);


/* used to call nextval on a sequence by going through the seqserver*/
extern void sendSequenceRequest(int sockfd, Oid dbid, Oid tablespaceid, 
								 Oid seq_oid, bool isTemp, int16 qd_pid,
								 int64   *plast, int64   *pcached,
			 					 int64   *pincrement);



#endif   /* SEQSERVER_H */
