/*-------------------------------------------------------------------------
 *
 * cdbmotion.c
 * 		Access into the motion-layer in order to send and receive tuples
 *	  		within a motion node ID within a particular process group id. 	
 *
 * Copyright (c) 2003-2004, Metapa
 *
 *
 * IDENTIFICATION
 *        $Id$
 *
 *
 *-------------------------------------------------------------------------
 */

#include "cdb/cdbmotion.h"




/*
 * Function:  SendTuple - Sends a portion or whole tuple to the AMS layer.
 *
 */
int 
SendTuple( MotionNodeInfo *mn_info, TupleSendContext *tupCtxt )
{
	//impl me!
	
	return 0;
}


/*
 * Function: SendEndOfStream - Signals other peer Motion Nodes that this motion
 *			       node has no more tuples to send out.	
 * 			
 */
 int  
 SendEndOfStream( MotionNodeInfo *mn_info ) 
 { 
 	return 0; 
 } 
 
 
/*
 * Function:
 *
 */
int 
RecvTuple( MotionNodeInfo *mn_info, HeapTuple *tup_i )
{
	return 0;

}
 
 
/*
 * Function:  CanReadTuple - blocks until a portion or whole tuple is ready to
 *			     to be read.
 *
 */
void 
CanReadTuple( )
{
	//TODO: eventually do some clever trick to monitor however the motion
	//	layer communicates with the AMS layer.  We want to block here
	//	until there is a portion of a tuple to be received. this will
	//	allow motion nodes that have already sent out their tuples
	//	to block until something interesting happens instead of having
	//	to poll or spin.
	// 
	
	//for now... we will do nothing.  :)

}
