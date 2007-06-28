/*-------------------------------------------------------------------------
* ml_ipc.h
*	Motion Layer IPC Layer.
*
* Copyright (c) 2005-2006, Greenplum
*-------------------------------------------------------------------------
*/
#ifndef ML_IPC_H
#define ML_IPC_H

#include "cdb/cdbselect.h"
#include "cdb/cdbinterconnect.h"
#include "cdb/cdbmotion.h"

struct SliceTable;                          /* #include "nodes/execnodes.h" */

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
 * shutdown of the process. This includes shutting down the Motion Listener.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
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
							  struct SliceTable * mySliceTable, 
							  int mySliceId,
							  MotionLayerState *mlStates);

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
								 MotionLayerState *mlStates,
								 bool forceEOS);



extern void SetupSequenceServer(struct SliceTable *mySliceTable);

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
extern bool SendTupleChunkToAMS(MotionLayerState *mlStates,
								ChunkTransportState *transportStates, 
								int16 motNodeID, 
								int16 targetRoute, 
								TupleChunkListItem tcItem);

/* The SendEosToAMS() function is used to send an "End Of Stream" message to
 * all connected receivers (generally this is a broadcast)
 *
 * PARAMETERS:
 *	 - motNodeID:	motion node Id that the tcItem belongs to.
 *	 - tcItem:		The tuple-chunk data to send.
 *
 */
extern void SendEosToAMS(MotionLayerState *mlStates,
						 ChunkTransportState *transportStates, 
						 int motNodeID, 
						 TupleChunkListItem tcItem);

/* The RecvTupleChunkFromAny() function attempts to receive one or more tuple
 * chunks from any of the incoming connections.  This function blocks until
 * at least one TupleChunk is received. (Although PG Interrupts are still
 * checked for within this call).
 *
 * This function makes some effort to "fairly" pull data from peers with data
 * available (a peer with data available is always better than waiting for
 * one without data available; but a peer with data available which hasn't been
 * read from recently is better than a peer with data available which has
 * been read from recently).
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
extern TupleChunkListItem RecvTupleChunkFromAny(MotionLayerState *mlStates,
												ChunkTransportState *transportStates, 
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
void
DeregisterReadInterest(ChunkTransportState *transportStates, 
                       int                  motNodeID, 
                       int                  srcRoute,
                       const char          *reason);

/*
 * doSendStopMessage() actually does the work of sending the stop message.
 */
extern void doSendStopMessage(ChunkTransportState *transportStates, 
							  int16 motNodeID);

#endif   /* ML_IPC_H */
