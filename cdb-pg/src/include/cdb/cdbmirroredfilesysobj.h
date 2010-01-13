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

extern void MirroredFileSysObj_CreateTablespaceDir(
	Oid		tablespace);

extern void MirroredFileSysObj_ScheduleDropTablespaceDir(
	Oid		tablespace);

extern void MirroredFileSysObj_DropTablespaceDir(
	Oid		tablespace,

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
