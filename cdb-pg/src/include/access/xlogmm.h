/*
 * prototypes for master mirror synchronisation write ahead logging.
 *
 * Copyright Greenplum, 2010.
 */
#ifndef MMXLOG_H
#define MMXLOG_H

/* 
 * We define this to make it clear where temporary master mirror sync code
 * resides. This is to be replaced by master file rep.
 */
#define MASTER_MIRROR_SYNC

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "lib/stringinfo.h"

#define MMXLOG_CREATE_DIR 0x00
#define MMXLOG_CREATE_FILE 0x10
#define MMXLOG_REMOVE_DIR 0x20
#define MMXLOG_REMOVE_FILE 0x30

typedef enum mm_fs_obj_type
{
	MM_OBJ_FILESPACE,
	MM_OBJ_TABLESPACE,
	MM_OBJ_DATABASE,
	MM_OBJ_RELFILENODE
} mm_fs_obj_type;

typedef struct xl_mm_fs_obj
{
	mm_fs_obj_type objtype; /* One of the MM_OBJ_* types */

	Oid filespace;
	Oid tablespace;
	Oid database;
	Oid relfilenode;
	uint32 segnum;

	/* union for alignment */
	union
	{
		struct
		{
			uint16 master;
			uint16 mirror;
		} dbid;
		int64 alignment;
	} u;

	char master_path[MAXPGPATH];
	char mirror_path[MAXPGPATH];
} xl_mm_fs_obj;

extern void mmxlog_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord *record);
extern void mmxlog_desc(StringInfo buf, XLogRecPtr beginLoc, XLogRecord *record);

/* 
 * Functions to generate WAL records to remove file system objects on the
 * master / standby master.
 */
extern void mmxlog_log_remove_filespace(Oid filespace);
extern void mmxlog_log_remove_tablespace(Oid tablespace);
extern void mmxlog_log_remove_database(Oid tablespace, Oid database);
extern void mmxlog_log_remove_relfilenode( Oid tablespace, Oid database,
										  Oid relfilenode, uint32 segnum);

/*
 * Functions to generate WAL records to add file system objects on the
 * master / standby master.
 */
extern void mmxlog_log_create_filespace(Oid filespace);
extern void mmxlog_log_create_tablespace(Oid filespace, Oid tablespace);
extern void mmxlog_log_create_database(Oid tablespace, Oid database);
extern void mmxlog_log_create_relfilenode(Oid tablespace, Oid database,
										  Oid relfilenode, uint32 segnum);
extern void mmxlog_append_checkpoint_data(XLogRecData rdata[4]);
extern void mmxlog_get_checkpoint_counts(char *cpdata, XLogRecPtr *beginLoc, int *tablespaceCount, int *filespaceCount);
extern void mmxlog_read_checkpoint_data(char *cpdata, XLogRecPtr *beginLoc);
extern bool mmxlog_filespace_get_path(
	Oid fspcoid,

	char **filespacePath);
extern bool mmxlog_tablespace_get_filespace(
	Oid tspcoid,

	Oid *filespaceOid);
#endif /* MMXLOG_H */
