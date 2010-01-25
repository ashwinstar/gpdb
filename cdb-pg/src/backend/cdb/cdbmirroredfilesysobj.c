/*-------------------------------------------------------------------------
 *
 * cdbmirroredfilesysobj.c
 *	  Create and drop mirrored files and directories.
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>

#include "cdb/cdbmirroredfilesysobj.h"
#include "cdb/cdbfilerepprimary.h"
#include "storage/smgr.h"

void MirroredFileSysObj_CreateTablespaceDir(
	TablespaceDirNode			*tablespaceDirNode)
{

//	if (FileRepPrimary_MirrorCreate(
//				FileRep_GetDirIdentifier(*dbDirNode),
//				FileRepRelationTypeDir) != 0) 
//		ereport(LOG,
//				(errmsg("could not sent create dbdir to mirror")));

	smgrcreatetablespacedir(tablespaceDirNode, false);
	
//	if (FileRepPrimary_IsOperationCompleted(
//				   FileRep_GetDirIdentifier(*dbDirNode),							
//				   FileRepRelationTypeDir) == FALSE)	
//		ereport(LOG,
//				(errmsg("could not create dbdir on mirror ")));					  
}

void MirroredFileSysObj_ScheduleDropTablespaceDir(
	TablespaceDirNode			*tablespaceDirNode)
{
	
	smgrschedulermtablespacedir(tablespaceDirNode);

}

void MirroredFileSysObj_DropTablespaceDir(
	TablespaceDirNode			*tablespaceDirNode,

	bool isRedo)
{

	// UNDONE: NOTE we don't mirror the drop yet in md.c:mdrmdbdir

//	if (FileRepPrimary_MirrorDrop(
//				FileRep_GetDirIdentifier(*dbDirNode),
//				FileRepRelationTypeDir) != 0) 
//		ereport(LOG,
//				(errmsg("could not sent drop dbdir to mirror")));
	

	smgrdormtablespacedir(tablespaceDirNode, isRedo);

//	if (FileRepPrimary_IsOperationCompleted(
//					FileRep_GetDirIdentifier(*dbDirNode),							
//					FileRepRelationTypeDir) == FALSE)	
//		ereport(LOG,
//				(errmsg("could not drop dbdir on mirror ")));	
}

void MirroredFileSysObj_CreateDbDir(
	DbDirNode			*dbDirNode)
{

	if (FileRepPrimary_MirrorCreate(
				FileRep_GetDirIdentifier(*dbDirNode),
				FileRepRelationTypeDir) != 0) 
		ereport(LOG,
				(errmsg("could not sent create dbdir to mirror")));

	smgrcreatedbdir(dbDirNode, false);
	
	if (FileRepPrimary_IsOperationCompleted(
				   FileRep_GetDirIdentifier(*dbDirNode),							
				   FileRepRelationTypeDir) == FALSE)	
		ereport(LOG,
				(errmsg("could not create dbdir on mirror ")));					  
}

void MirroredFileSysObj_ScheduleDropDbDir(
	DbDirNode			*dbDirNode)
{
	
	smgrschedulermdbdir(dbDirNode);

}

void MirroredFileSysObj_DropDbDir(
	DbDirNode			*dbDirNode,

	bool isRedo)
{

	// UNDONE: NOTE we don't mirror the drop yet in md.c:mdrmdbdir

	if (FileRepPrimary_MirrorDrop(
				FileRep_GetDirIdentifier(*dbDirNode),
				FileRepRelationTypeDir) != 0) 
		ereport(LOG,
				(errmsg("could not sent drop dbdir to mirror")));
	

	smgrdormdbdir(dbDirNode, isRedo);

	if (FileRepPrimary_IsOperationCompleted(
					FileRep_GetDirIdentifier(*dbDirNode),							
					FileRepRelationTypeDir) == FALSE)	
		ereport(LOG,
				(errmsg("could not drop dbdir on mirror ")));	
}

void MirroredFileSysObj_CreateRelFile(
	SMgrRelation smgrOpen,

	ItemPointer persistentTid,

	int64	*persistentSerialNum,

	bool isLocalBuf)
{

	if (FileRepPrimary_MirrorCreate(
				FileRep_GetRelationIdentifier(smgrOpen->smgr_rnode, 0),
				FileRepRelationTypeFile) != 0) 
		ereport(LOG,
				(errmsg("could not sent create relfile to mirror")));
	
	smgrcreate(smgrOpen, persistentTid, persistentSerialNum, isLocalBuf, /* isRedo */ false);


	if (FileRepPrimary_IsOperationCompleted(
					FileRep_GetRelationIdentifier(smgrOpen->smgr_rnode, 0),							
					FileRepRelationTypeFile) == FALSE)	
		ereport(LOG,
				(errmsg("could not create relfile on mirror ")));			
}

void MirroredFileSysObj_CreateOnlyRelFile(
	SMgrRelation smgrOpen,

	bool isLocalBuf)
{

	if (FileRepPrimary_MirrorCreate(
				FileRep_GetRelationIdentifier(smgrOpen->smgr_rnode, 0),
				FileRepRelationTypeFile) != 0) 
		ereport(LOG,
				(errmsg("could not sent create relfile to mirror")));

	// UNDONE: For now, just set the isRedo flag to indicate we've already have the 'Create Pending'
	// UNDONE: persistent entry...
	smgrcreate(smgrOpen, NULL, NULL, isLocalBuf, /* isRedo */ true);


	if (FileRepPrimary_IsOperationCompleted(
					FileRep_GetRelationIdentifier(smgrOpen->smgr_rnode, 0),							
					FileRepRelationTypeFile) == FALSE)	
		ereport(LOG,
				(errmsg("could not create relfile on mirror ")));			
}

void MirroredFileSysObj_ScheduleDropRelFile(
	SMgrRelation smgrOpen,

	ItemPointer persistentTid,

	int64	persistentSerialNum,

	bool isLocalBuf)
{

	smgrscheduleunlink(smgrOpen, persistentTid, persistentSerialNum, isLocalBuf);
}

void MirroredFileSysObj_DropRelFile(
	SMgrRelation smgrOpen,

	bool isLocalBuf,

	bool isRedo)
{

	smgrdounlink(smgrOpen, isLocalBuf, isRedo);

}

