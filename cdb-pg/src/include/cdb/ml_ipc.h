/*-------------------------------------------------------------------------
* ml_ipc.h
*	Motion Layer IPC Layer.
*
* Copyright (c) 2005, Greenplum
*-------------------------------------------------------------------------
*/
#ifndef ML_IPC_H
#define ML_IPC_H

#include "nodes/primnodes.h"
#include "cdb/tupchunklist.h"
#include "nodes/execnodes.h"

typedef enum MotionConnState
{
    mcsNull,
    mcsAccepted,
    mcsSetupOutgoingConnection,
    mcsConnecting,
    mcsRecvRegMsg,
    mcsSendRegMsg,
    mcsStarted,
} MotionConnState;

/* Structure used for keeping track of a pt-to-pt connection between two
* Cdb Entities (either QE or QD).
*/
typedef struct MotionConn
{
	/* socket file descriptor. */
	int			sockfd;
	
	/* pointer to the data buffer. */
	uint8	   *pBuff;
	
	/* size of the message in the buffer, if any. */
	int32		msgSize;
	
	/* position of message inside of buffer, "cursor" pointer */
	uint8	   *msgPos;
	
	/*
	 * recv bytes: we can have more than one message/message fragment in recv
	 * queue at once
	 */
	int32		recvBytes;
	
	int			tupleCount;
	
	bool		stillActive;
	
    MotionConnState state;
	
    int         wakeup_ms;
	
    CdbProcess *cdbProc;
    int			remoteContentId;
    char        remoteHostAndPort[64];
    char        localHostAndPort[64];
	
}	MotionConn;

/* used to organize all of the information for a given motion node.
*
*/
typedef struct ChunkTransportStateEntry
{
	int         motNodeId;
	bool		valid;
	
	/* Connection array: first the primaries, then the mirrors (if needed) */
    MotionConn *conns;
	int			numConns;               /* all, including mirrors if present */
    int         numPrimaryConns;        /* does not include mirrors */
	
	/*
	 * used for receiving. to select() from a set of interesting MotionConns
	 * to see when data is ready to be read.  When the incoming connections
	 * are established, read interest is turned on.  It is turned off when an
	 * EOS (End of Stream) message is read.
	 */
	fd_set		readSet;
	
	/* highest file descriptor in the readSet. */
	int			highReadSock;
    int         scanStart;
	
    /* slice table entries */
    Slice      *sendSlice;
    Slice      *recvSlice;
	
    /* setup info */
    int         outgoingPortRetryCount;
	
}	ChunkTransportStateEntry;

typedef struct ChunkTransportState
{
	int size;
	ChunkTransportStateEntry *states;
} ChunkTransportState;

/*
 * Registration message
 *
 * Upon making a connection, the sender sends a registration message to
 * identify itself to the receiver.  A lot of the fields are just there
 * for validity checking.
 */
typedef struct RegisterMessage
{
	int32       msgBytes;
    int32       recvSliceIndex;
    int32       sendSliceIndex;
	int32       srcContentId;
	int32		srcListenerPort;
	int32		srcPid;
    int32       srcSessionId;
    int32       srcCommandCount;
}	RegisterMessage;

/* 2 bytes to store the size of the entire packet.	a packet is composed of
 * of one or more serialized TupleChunks (each of which has a TupleChunk
 * header.
 */
#define PACKET_HEADER_SIZE 4

/* Performs initialization of the MotionLayerIPC.  This should be called before
 * any work is performed through functions here.  Generally, this should only
 * need to be called only once during process startup.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * by a return code.
 *
 */
extern void InitMotionLayerIPC(void);

/* Performs any cleanup necessary by the Motion Layer IPC.	This is the cleanup
 * function that matches InitMotionLayerIPC, it should only be called during
 * shutdown of the process.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 */
extern void CleanUpMotionLayerIPC(void);

/* Returns the fd of the socket that connects to the seqserver.  This value
 * is -1 if it has not been setup.
 */
extern int  GetSeqServerFD(void);

/* The SetupInterconnect() function should be called at the beginning of
 * executing any DML statement that will need to use the interconnect.
 *
 * This function goes through the slicetable and makes any appropriate
 * outgoing connections as well as accepts any incoming connections.  Incoming
 * connections will have a "Register" message from them to see which remote
 * CdbProcess sent it.
 *
 * So this function essentially performs all of the setup the interconnect has
 * to perform for all of the motion nodes in the upcoming DML statement.
 *
 * PARAMETERS
 *
 *	 mySliceTable - slicetable structure that correlates to the upcoming DML
 *					statement.
 *
 *	 mySliceId - the index of the slice in the slicetable that we are a member of.
 *
 */
extern void SetupInterconnect(ChunkTransportState **transportStates, 
							  SliceTable * mySliceTable, 
							  int mySliceId);

/* The TeardownInterconnect() function should be called at the end of executing
 * a DML statement to close down all TCP socket resources that were setup during
 * SetupInterconnect().
 *
 * NOTE: it is important that TeardownInterconnect() happens
 *		 regardless of the outcome of the statement. i.e. gets called
 *		 even if an ERROR occurs during the statement. For abnormal
 *		 statement termination we can force an end-of-stream notification.
 *
 */
extern void TeardownInterconnect(ChunkTransportState *transportStates, 
								 bool forceEOS);



extern void SetupSequenceServer(SliceTable *mySliceTable);

extern void TeardownSequenceServer(void);


/* Sends a tuple chunk from the Postgres process to the local AMS process via
 * IPC.  This function does not block; if the IPC channel cannot accept the
 * tuple chunk for some reason, then this is indicated by a return-code.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 *
 * PARAMETERS:
 *	 - motNodeID:	motion node Id that the tcItem belongs to.
 *	 - targetRoute: route to send this tcItem out over.
 *	 - tcItem:		The tuple-chunk data to send.
 *
 */
extern bool SendTupleChunkToAMS(ChunkTransportState *transportStates, 
								int16 motNodeID, 
								int16 targetRoute, 
								TupleChunkListItem tcItem);

/* The SendEosToAMS() function is used to send an "End Of Stream" message to
 * the specified targetRoute. (generally this is a broadcast)
 *
 * PARAMETERS:
 *	 - motNodeID:	motion node Id that the tcItem belongs to.
 *	 - targetRoute: route to send this tcItem out over.
 *	 - tcItem:		The tuple-chunk data to send.
 *
 */
extern void SendEosToAMS(ChunkTransportState *transportStates, 
						 int16 motNodeID, 
						 int16 targetRoute, 
						 TupleChunkListItem tcItem);

/* The RecvTupleChunkFromAny() function attempts to receive one or more tuple
 * chunks from any of the incoming connections.  This function blocks until
 * at least one TupleChunk is received. (Although PG Interrupts are still
 * checked for within this call).
 *
 * NOTE: The TupleChunkListItem can have other's chained to it.  The caller
 *		 should check and process all in list.
 *
 * PARAMETERS:
 *	- motNodeID:  motion node id to receive for.
 *	- srcRoute: output parameter that allows the function to return back which
 *				route the TupleChunkListItem is from.
 *
 * RETURN:
 *	 - A populated TupleChunkListItemData structure (allocated with palloc()).
 */
extern TupleChunkListItem RecvTupleChunkFromAny(ChunkTransportState *transportStates, 
												int16 motNodeID, 
												int16 *srcRoute);


/* The RecvTupleChunkFrom() function is similar to the RecvTupleChunkFromAny()
 * function except that the connection we are interested in is specified with
 * srcRoute.
 *
 * PARAMETERS:
 *	 - motNodeID: motion node id to receive for.
 *	 - srcRoute:  which connection to receive on.
 * RETURN:
 *	 - A populated TupleChunkListItemData structure (allocated with palloc()).
 */
extern TupleChunkListItem RecvTupleChunkFrom(ChunkTransportState *transportStates, 
											 int16 motNodeID, 
											 int16 srcRoute);

/* The DeregisterReadInterest() function is used to specify that we are no
 * longer interested in reading from the specified srcRoute. After calling this
 * function, we should no longer ever return TupleChunks from this srcRoute
 * when calling RecvTupleChunkFromAny().
 *
 * PARAMTERS:
 *	 - motNodeID: motion node id that this applies to.
 *	 - srcRoute:  which connection to turn off reads for.
 *
 */
extern void DeregisterReadInterest(ChunkTransportState *transportStates, 
								   int16 motNodeID, 
								   int16 srcRoute);

/*
 * doSendStopMessage() actually does the work of sending the stop message.
 */
extern void doSendStopMessage(ChunkTransportState *transportStates, 
							  int16 motNodeID);

#endif   /* ML_IPC_H */
