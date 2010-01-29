/*-------------------------------------------------------------------------
 *
 * cdbmirroredfsobj.h
 *	  Create and drop mirrored files and directories.
 *
 * Copyright (c) 2009-2010, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBMIRROREDFSOBJ_H
#define CDBMIRROREDFSOBJ_H

#include "postgres.h"
#include "storage/relfilenode.h"
#include "storage/dbdirnode.h"
#include "storage/smgr.h"

extern void MirroredFileSysObj_CreateFilespaceDir(
	Oid			filespaceOid,

	int16		primaryDbId,
	
	char		*primaryFilespaceLocation,
				/* 
				 * The primary filespace directory path.  NOT Blank padded.
				 * Just a NULL terminated string.
				 */
	
	int16		mirrorDbId,
	
	char		*mirrorFilespaceLocation);

extern void MirroredFileSysObj_ScheduleDropFilespaceDir(
	Oid							filespaceOid);

extern void MirroredFileSysObj_DropFilespaceDir(
	Oid			filespaceOid,

	char		*primaryFilespaceLocation,
				/* 
				 * The primary filespace directory path.  NOT Blank padded.
				 * Just a NULL terminated string.
				 */
	
	char		*mirrorFilespaceLocation,

	bool isRedo);

extern void MirroredFileSysObj_CreateTablespaceDir(
	TablespaceDirNode			*tablespaceDirNode);

extern void MirroredFileSysObj_ScheduleDropTablespaceDir(
	Oid							tablespaceOid);

extern void MirroredFileSysObj_DropTablespaceDir(
	Oid			tablespaceOid,

	bool	isRedo);

extern void MirroredFileSysObj_CreateDbDir(
	DbDirNode		*dbDirNode);

extern void MirroredFileSysObj_ScheduleDropDbDir(
	DbDirNode		*dbDirNode);

extern void MirroredFileSysObj_DropDbDir(
	DbDirNode		*dbDirNode,

	bool	isRedo);

extern void MirroredFileSysObj_CreateRelFile(
	SMgrRelation smgrOpen,

	ItemPointer persistentTid,

	int64	*persistentSerialNum,

	bool isLocalBuf);

extern void MirroredFileSysObj_CreateOnlyRelFile(
	SMgrRelation smgrOpen,

	bool isLocalBuf);

extern void MirroredFileSysObj_ScheduleDropRelFile(
	SMgrRelation smgrOpen,

	ItemPointer persistentTid,

	int64	persistentSerialNum,

	bool isLocalBuf);

extern void MirroredFileSysObj_DropRelFile(
	SMgrRelation smgrOpen,

	bool isLocalBuf,

	bool isRedo);


#endif   /* CDBMIRROREDFSOBJ_H */
