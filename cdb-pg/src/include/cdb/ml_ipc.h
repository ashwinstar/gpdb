/*-------------------------------------------------------------------------
* ml_ipc.h
*	Motion Layer IPC Layer.     
*
* Copyright (c) 2005, Metapa
*-------------------------------------------------------------------------
*/
#ifndef ML_IPC_H
#define ML_IPC_H

#include "cdb/tupchunklist.h"


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


/* Sends a tuple chunk from the Postgres process to the local AMS process via
 * IPC.  This function does not block; if the IPC channel cannot accept the
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


/* Receives a tuple chunk from the local AMS process via IPC.  This function
 * does not block; if there is no tuple chunk available, then NULL is
 * returned.
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

#endif /* ML_IPC_H */