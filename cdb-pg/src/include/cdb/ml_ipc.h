/*-------------------------------------------------------------------------
* ml_ipc.h
*	Motion Layer IPC Layer.     
*
* Copyright (c) 2005, Metapa
*-------------------------------------------------------------------------
*/
#ifndef ML_IPC_H
#define ML_IPC_H

#include <mpi.h>

#include "cdb/tupchunklist.h"


extern void setMpiComm( MPI_Comm comm );

/* Performs initialization of the MotionLayerIPC.  This should be called before
 * any work is performed through functions here.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 * PARAMETERS:
 *   - segmentId:  The segmentId for this SegmentDatabase.
 *
 * RETURN:
 *   - none.
 */
extern void InitMotionLayerIPC( int segmentId );


/* Performs any cleanup necessary by the Motion Layer IPC.  The cleanup is for
 * connection level cleanup that should happen during connection teardown.
 *
 * TODO: is this statement level or connection level?
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 * PARAMETERS:
 *   - none.
 *
 * RETURN:
 *   - none.
 */
extern void CleanUpMotionLayerIPC(void);


/* Sends a tuple chunk from the Postgres process to the local AMS process via
 * IPC.  This function does not block; if the IPC channel cannot accept the
 * tuple chunk for some reason, then this is indicated by a return-code.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 * PARAMETERS:
 *   - tcItem:  The tuple-chunk data to send.
 *
 * RETURN:
 *   - false if the chunk could not be sent, or true if it could be sent.
 */
extern bool SendTupleChunkToAMS(TupleChunkListItem tcItem);


/* Attempts to Receive one or more tuple chunks from the local AMS process
 * via IPC.  This function does not block; if there is no tuple chunk available,
 * does not block; then NULL is returned.  
 * 
 * NOTE: Although this function returns a chain of TupleChunkListItem's, they
 *       are not necessarily all for the same motion node.  The caller of this
 *       function will have to go through and put them back together as needed.
 *
 * Errors are indicated by calls to ereport(), and are therefore not indicated
 * in the return code.
 *
 * PARAMETERS:
 *   - none.
 *
 * RETURN:
 *   - A populated TupleChunkListItemData structure (allocated with palloc()),
 *     or NULL if no chunk is available at time of call.
 */
extern TupleChunkListItem RecvTupleChunkFromAMS(void);




/* Allows the caller to wait on a combination of specified read and write
 * interest.   Read interest is further specified by a segId.  (one can
 * specify ALL to by notified of a read for any).
 *
 * Initially, (while we are without flow control), we should disallow the
 * segId to be of any value other than ALL.
 * 
 *
 * PARAMETERS:
 *
 *  - readInterest:  signals if interested in knowing when to read again.
 *
 *  - readSegId:  significant only when readInterest is true.  Specifies which
 *                segId we are interested in reading. This is an IN/OUT param.
 *				  although currently the OUT part is not implemented.
 *				  
 *
 *  - writeInterest:  signals if interested in knowing when we can write
 *  			      again.
 *
 *  - writeSegId: significant only when writeInterest is true. specifies
 *				  which segIds we can write to. This is an IN/OUT param but
 *				  the OUT part is not implemented.
 */
 extern void waitForMLIpc( bool readInterest, int *readSegId, 
						   bool writeInterest, int *writeSegId );

#endif /* ML_IPC_H */
