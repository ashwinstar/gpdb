/*
 *  cdbfilerepservice.h
 *  
 *
 *  Copyright 2009-2010, Greenplum Inc. All rights reserved.
 *
 */

#ifndef CDBFILEREPSERVICE_H
#define CDBFILEREPSERVICE_H

#include "cdb/cdbfilerep.h"



extern FileRepProcessType_e fileRepProcessType;
/*
extern FileRepRole_e		fileRepRole;
extern SegmentState_e		segmentState;
extern DataState_e			dataState;
*/
extern void FileRepSubProcess_Main(void);

extern void FileRepSubProcess_SetState(
					FileRepState_e fileRepStateLocal);

/*
 * Return state of FileRep process.
 */
extern FileRepState_e FileRepSubProcess_GetState(void);


/*
 * Process signals 
 * Return TRUE if shutdown was requested
 */	
extern bool FileRepSubProcess_ProcessSignals(void);


#endif   /* CDBFILEREPSERVICE_H */

