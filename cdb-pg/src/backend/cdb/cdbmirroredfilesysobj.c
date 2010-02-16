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
#include "storage/smgr.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_database.h"
#include "catalog/gp_persistent.h"
#include "cdb/cdbpersistentdatabase.h"
#include "cdb/cdbpersistenttablespace.h"
#include "storage/lwlock.h"
#include "miscadmin.h"

void MirroredFileSysObj_CreateFilespaceDir(
	Oid			filespaceOid,

	int16		primaryDbId,
	
	char		*primaryFilespaceLocation,
				/* 
				 * The primary filespace directory path.  NOT Blank padded.
				 * Just a NULL terminated string.
				 */
	
	int16		mirrorDbId,
	
	char		*mirrorFilespaceLocation)
{
	Assert(primaryFilespaceLocation != NULL);

	smgrcreatefilespacedir(filespaceOid, primaryDbId, primaryFilespaceLocation, mirrorDbId, mirrorFilespaceLocation, false);
}

void MirroredFileSysObj_ScheduleDropFilespaceDir(
	Oid			filespaceOid)
{
	smgrschedulermfilespacedir(filespaceOid);
}

void MirroredFileSysObj_DropFilespaceDir(
	Oid			filespaceOid,

	char		*primaryFilespaceLocation,
				/* 
				 * The primary filespace directory path.  NOT Blank padded.
				 * Just a NULL terminated string.
				 */
	
	char		*mirrorFilespaceLocation,

	bool isRedo)
{
	Assert(primaryFilespaceLocation != NULL);

	smgrdormfilespacedir(filespaceOid, primaryFilespaceLocation, mirrorFilespaceLocation, isRedo);
}


void MirroredFileSysObj_CreateTablespaceDir(
	TablespaceDirNode			*tablespaceDirNode)
{
	Assert(tablespaceDirNode != NULL);

	smgrcreatetablespacedir(tablespaceDirNode, false);
}

void MirroredFileSysObj_ScheduleDropTablespaceDir(
	Oid			tablespaceOid)
{
	smgrschedulermtablespacedir(tablespaceOid);
}

void MirroredFileSysObj_DropTablespaceDir(
	Oid			tablespaceOid,

	bool isRedo)
{
	smgrdormtablespacedir(tablespaceOid, isRedo);
}

void MirroredFileSysObj_CreateDbDir(
	DbDirNode			*dbDirNode)
{
	Assert(dbDirNode != NULL);

	smgrcreatedbdir(
				dbDirNode, 
				/* isRedo */ false);
}

void MirroredFileSysObj_ScheduleDropDbDir(
	DbDirNode			*dbDirNode)
{
	Assert(dbDirNode != NULL);

	smgrschedulermdbdir(dbDirNode);

}

void MirroredFileSysObj_DropDbDir(
	DbDirNode			*dbDirNode,

	bool isRedo)
{
	Assert(dbDirNode != NULL);

	smgrdormdbdir(dbDirNode, isRedo);
}

static void MirroredFileSysObj_JustInTimeDbDirCreate(
	DbDirNode			*justInTimeDbDirNode)
{
	Assert(justInTimeDbDirNode != NULL);

	if (justInTimeDbDirNode->tablespace == GLOBALTABLESPACE_OID ||
		(justInTimeDbDirNode->tablespace == DEFAULTTABLESPACE_OID &&
		 justInTimeDbDirNode->database == TemplateDbOid))
	{
		/*
		 * Optimize out the common cases.
		 */
		 return;
	}

	/*
	 * Acquire TablespaceCreateLock to ensure that no DROP TABLESPACE
	 * or MirroredFileSysObj_JustInTimeDbDirCreate is running concurrently.
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

	/* Prevent cancel/die interrupt while doing this multi-step */
	HOLD_INTERRUPTS();

	if (PersistentDatabase_DirIsCreated(justInTimeDbDirNode))
	{
		RESUME_INTERRUPTS();
		LWLockRelease(TablespaceCreateLock);
		return;
	}

	if (!smgrcreatedbdirjustintime(justInTimeDbDirNode))
	{
		RESUME_INTERRUPTS();
		LWLockRelease(TablespaceCreateLock);
		ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not create database directory %u/%u: %m",
					justInTimeDbDirNode->tablespace,
					justInTimeDbDirNode->database)));
	}
	
	RESUME_INTERRUPTS();

	LWLockRelease(TablespaceCreateLock);
}

void MirroredFileSysObj_CreateRelFile(
	SMgrRelation smgrOpen,

	ItemPointer persistentTid,
				/* Output: The TID of the gp_persistent_relation_node tuple. */

	int64	*persistentSerialNum,
				/* Output: The serial number of the gp_persistent_relation_node tuple. */

	bool isLocalBuf)
{
	// UNDONE: This is broken -- not sure why (MPP-7949, etc).
	DbDirNode justInTimeDbDirNode;

	Assert(smgrOpen != NULL);
	Assert(persistentTid != NULL);
	Assert(persistentSerialNum != NULL);

	/*
	 * "Fault-in" the database directory in a tablespace if it doesn't exist yet.
	 */
	justInTimeDbDirNode.tablespace = smgrOpen->smgr_rnode.spcNode;
	justInTimeDbDirNode.database = smgrOpen->smgr_rnode.dbNode;
	MirroredFileSysObj_JustInTimeDbDirCreate(&justInTimeDbDirNode);

	/*
	 * We write our intention or 'Create Pending' persistent information before we do
	 * any create relation work on either the primary or mirror.
	 */
	smgrcreatepending(
		&smgrOpen->smgr_rnode, 
		persistentTid, 
		persistentSerialNum, 
		isLocalBuf, 
		/* flushToXLog */ true);

	/*
	 * Synchronous primary and mirror create relation.
	 */
	MirroredFileSysObj_CreateOnlyRelFile(
									smgrOpen,
									isLocalBuf);
}

void MirroredFileSysObj_CreateOnlyRelFile(
	SMgrRelation smgrOpen,

	bool isLocalBuf)
{
	Assert(smgrOpen != NULL);

	smgrcreate(
			smgrOpen, 
			isLocalBuf, 
			/* isRedo */ false);
}

void MirroredFileSysObj_ScheduleDropRelFile(
	SMgrRelation smgrOpen,

	ItemPointer persistentTid,

	int64	persistentSerialNum,

	bool isLocalBuf)
{
	Assert(smgrOpen != NULL);
	Assert(persistentTid != NULL);

	smgrscheduleunlink(smgrOpen, persistentTid, persistentSerialNum, isLocalBuf);
}

void MirroredFileSysObj_DropRelFile(
	SMgrRelation smgrOpen,

	bool isLocalBuf,

	bool isRedo)
{
	Assert(smgrOpen != NULL);

	smgrdounlink(smgrOpen, isLocalBuf, isRedo);

}

