/*-------------------------------------------------------------------------
 *
 * xlog_mm.c
 *
 * Special xlog handling for master mirroring.
 *
 * Copyright (c) 2010 Greenplum
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "postgres.h"

#include "access/xlogmm.h"
#include "catalog/gp_segment_config.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_tablespace.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "commands/filespace.h"
#include "commands/tablespace.h"
#include "utils/builtins.h"

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

static void tblspc_get_filespace_paths(Oid tblspc, int16 master_dbid, 
									   int16 mirror_dbid,
									   char **master_path, char **mirror_path);
static void get_filespace_paths(Oid filespace, char **master_path, char **mirror_path);

/* extract the object path based on whether we're a mirror or master */
static char *
obj_get_path(xl_mm_fs_obj *xlrec)
{
	char *path = NULL;

	if (xlrec->u.dbid.mirror == GpIdentity.dbid)
		path = xlrec->mirror_path;
	else if (xlrec->u.dbid.master == GpIdentity.dbid)
		path = xlrec->master_path;
	else
		elog(PANIC, "mmxlog record not for my dbid %u - [%u, %u]",
			 GpIdentity.dbid, xlrec->u.dbid.master, xlrec->u.dbid.mirror);

	return path;
}



/* 
 * Unlink the object referenced by path. Tolerate it not existing but
 * do not tolerate any other error.
 *
 * For directories, we remove the whole tree underneath the directory and then
 * the directory itself.
 */
static void
unlink_obj(char *path, uint8 info)
{
	if (info == MMXLOG_REMOVE_DIR)
	{
		/* same behaviour as dropdb(), RemoveFileSpace(), RemoveTableSpace() */
		rmtree(path, true);
	}
	else if (info == MMXLOG_REMOVE_FILE)
	{
		if (unlink(path) < 0)
		{
			if (errno != ENOENT) /* allow it to already be removed */
				elog(PANIC, "could not unlink %s: %m",
					 path);
		}
	}
	else
		Insist(false);
}

/* Actual replay code */
void
mmxlog_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	xl_mm_fs_obj *xlrec = (xl_mm_fs_obj *) XLogRecGetData(record);
	char *path = obj_get_path(xlrec);

	if (info == MMXLOG_CREATE_DIR)
	{

		elog(DEBUG1, "got create directory request: %s / %s",
			 xlrec->master_path,
			 xlrec->mirror_path);

		/* 
		 * Inform the persistent table code about the new filespace or
		 * tablespace.
		 */
		if (xlrec->objtype == MM_OBJ_FILESPACE)
			xlog_persistent_filespace_create(xlrec->filespace,
											 xlrec->u.dbid.master,
											 xlrec->master_path,
											 xlrec->u.dbid.mirror,
											 xlrec->mirror_path);

		else if (xlrec->objtype == MM_OBJ_TABLESPACE)
			xlog_persistent_tablespace_create(xlrec->filespace,
											  xlrec->tablespace);

		if (mkdir(path, 0700) < 0)
		{
			/* allowed to already exist */
			if (errno != EEXIST)
				elog(PANIC, "could not create directory %s: %m", path);
		}

		/* need to add PG_VERSION for newly created databases */
		if (xlrec->objtype == MM_OBJ_DATABASE)
			set_short_version(path, NULL, false);
	}
	else if (info == MMXLOG_REMOVE_DIR)
	{
		unlink_obj(path, info);
	}
	else if (info == MMXLOG_CREATE_FILE)
	{
		int fd = open(path, O_CREAT | O_RDWR | PG_BINARY, 0600);

		if (fd < 0)
		{
			/* tolerate existence */
			if (errno != EEXIST)
				elog(PANIC, "could open open file %s: %m", path);
		}

		/*
		 * Yes, close(2) can fail! The only interesting error here is
		 * that we were interrupted. Don't leak the file descriptor
		 */
		while (close(fd) < 0 && errno == EINTR)
			/* loop */
			;

	}
	else if (info == MMXLOG_REMOVE_FILE)
	{
		unlink_obj(path, info);
	}
	else
		elog(PANIC, "unknown mmxlog op code %u", info);
}

/*
 * For log output
 */
void
mmxlog_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;
	xl_mm_fs_obj *xlrec = (xl_mm_fs_obj *) rec;
	char *path = obj_get_path(xlrec); 

	if (info == MMXLOG_CREATE_DIR)
		appendStringInfo(buf, "create directory: %s", path);
	else if (info == MMXLOG_REMOVE_DIR)
		appendStringInfo(buf, "remove directory: %s", path);
	else if (info == MMXLOG_CREATE_FILE)
		appendStringInfo(buf, "create file: %s", path);
	else if (info == MMXLOG_REMOVE_FILE)
		appendStringInfo(buf, "remove file: %s", path);
	else
		appendStringInfo(buf, "UNKNOWN");
}

/* convert and oid to a string */
static char *
oidtoa(Oid oid)
{
	char *tmp = palloc(11); /* maximum OID is UINT_MAX */

	pg_ltoa(oid, tmp);

	return tmp;
}

static char *
append_file_parts(mm_fs_obj_type type,
				  char *str, Oid tablespace, Oid database, Oid relfilenode,
				  uint32 segnum)
{
	if (type == MM_OBJ_FILESPACE)
		return str; /* already done in base path */

	/* first two are the special tablespaces */
	if (tablespace == DEFAULTTABLESPACE_OID)
		join_path_components(str, str, "base");
	else if (tablespace == GLOBALTABLESPACE_OID)
		join_path_components(str, str, "global");
	else
		join_path_components(str, str, oidtoa(tablespace));

	if (OidIsValid(database))
		join_path_components(str, str, oidtoa(database));

	if (OidIsValid(relfilenode))
	{
		join_path_components(str, str, oidtoa(relfilenode));

		/* Is this a segment > 0 ? If so, add the segment file number */
		if (segnum)
			sprintf(str + strlen(str), ".%u", segnum);
	}

	return str;
}

/*
 * Guts of logging for creation or destruction of filesystem objects on
 * the master.
 */
static void
emit_mmxlog_fs_record(mm_fs_obj_type type, Oid filespace,
					  Oid tablespace, Oid database, Oid relfilenode,
					  uint32 segnum, uint8 flags)
{
	if (InRecovery)
		/* no business here */
		return;

	/* only interesting on the master */
	if (GpIdentity.dbid == MASTER_DBID)
	{
		XLogRecData rdata;
		xl_mm_fs_obj xlrec;
		char *master_path, *mirror_path;
		int16 master_dbid, mirror_dbid;
		
		Insist(Gp_role == GP_ROLE_DISPATCH ||
			   Gp_role == GP_ROLE_UTILITY);

		master_path = NULL;
		mirror_path = NULL;
		master_dbid = GpIdentity.dbid;
		mirror_dbid = my_mirror_dbid();

		if (type == MM_OBJ_FILESPACE)
		{
			Insist(OidIsValid(filespace));
			get_filespace_paths(filespace, &master_path, &mirror_path);
		}
		else
		{
			Insist(OidIsValid(tablespace));
			tblspc_get_filespace_paths(tablespace, master_dbid, mirror_dbid,
									   &master_path, &mirror_path);

			filespace = PersistentTablespace_GetFileSpaceOid(tablespace);
		}

		Insist(PointerIsValid(master_path));
		
		if (mirror_dbid > 0)
		{
			Insist(PointerIsValid(mirror_path));
		}

		/*
		 * Make a non-transactional XLOG entry showing the file creation. It's
		 * non-transactional because we should replay it whether the transaction
		 * commits or not; if not, the file will be dropped at abort time.
		 */
		xlrec.objtype = type;
		xlrec.filespace = filespace;
		xlrec.tablespace = tablespace;
		xlrec.database = database;
		xlrec.relfilenode = relfilenode;
		xlrec.segnum = segnum;

		xlrec.u.dbid.master = master_dbid;
		xlrec.u.dbid.mirror = mirror_dbid;

		Insist(strlen(master_path) <= MAXPGPATH);
		strncpy(xlrec.master_path, master_path,
				sizeof(xlrec.master_path));

		append_file_parts(type, xlrec.master_path, tablespace, database,
						  relfilenode, segnum);

		if (mirror_dbid)
		{

			strncpy(xlrec.mirror_path, mirror_path,
					sizeof(xlrec.mirror_path));
			append_file_parts(type, xlrec.mirror_path, tablespace, database,
							  relfilenode, segnum);
		}
		else
			xlrec.mirror_path[0] = '\0';
		
		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;
		
		(void)XLogInsert(RM_MMXLOG_ID, flags | XLOG_NO_TRAN, &rdata);
	}
}

/* External interface to filespace removal logging */
void
mmxlog_log_remove_filespace(Oid filespace)
{
	emit_mmxlog_fs_record(MM_OBJ_FILESPACE,
						  filespace,
						  InvalidOid /* tablespace */,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_REMOVE_DIR);
}

/* External interface to tablespace removal logging */
void
mmxlog_log_remove_tablespace(Oid tablespace)
{
	emit_mmxlog_fs_record(MM_OBJ_TABLESPACE,
						  InvalidOid /* filespace */,
						  tablespace,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_REMOVE_DIR);
}

/* External interface to database removal logging */
void
mmxlog_log_remove_database(Oid tablespace, Oid database)
{
	emit_mmxlog_fs_record(MM_OBJ_DATABASE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_REMOVE_DIR);
}

/* External interface to relfilenode removal logging */
void
mmxlog_log_remove_relfilenode(Oid tablespace, Oid database, Oid relfilenode,
							  uint32 segnum)
{
	emit_mmxlog_fs_record(MM_OBJ_RELFILENODE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  relfilenode,
						  segnum,
						  MMXLOG_REMOVE_FILE);
}

/* External interface to filespace creation logging */
void
mmxlog_log_create_filespace(Oid filespace)
{
	emit_mmxlog_fs_record(MM_OBJ_FILESPACE,
						  filespace,
						  InvalidOid /* tablespace */,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_CREATE_DIR);
}

/* External interface to tablespace creation logging */
void
mmxlog_log_create_tablespace(Oid tablespace)
{
	emit_mmxlog_fs_record(MM_OBJ_TABLESPACE,
						  InvalidOid /* filespace */,
						  tablespace,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_CREATE_DIR);
}

/* External interface to database creation logging */
void
mmxlog_log_create_database(Oid tablespace, Oid database)
{
	emit_mmxlog_fs_record(MM_OBJ_DATABASE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_CREATE_DIR);
}

/* External interface to relfilenode creation logging */
void
mmxlog_log_create_relfilenode(Oid tablespace, Oid database,
							  Oid relfilenode, uint32 segnum)
{
	emit_mmxlog_fs_record(MM_OBJ_RELFILENODE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  relfilenode,
						  segnum,
						  MMXLOG_CREATE_FILE);
}

/*
 * Given a filespace OID, get the master and mirror filespace paths. We need
 * to do this via the persistent filespace table and not the pg_filespace table
 * because the entry in the latter table is likely already gone.
 */
static void
get_filespace_paths(Oid filespace, char **master_path, char **mirror_path)
{
	PersistentFilespace_GetPrimaryAndMirror(filespace, master_path, mirror_path);
}

/*
 * Given a tablespace OID, get the master and mirror filespace paths.
 */
static void
tblspc_get_filespace_paths(Oid tblspc, int16 master_dbid, int16 mirror_dbid,
						   char **master_path, char **mirror_path)
{
	/* 
	 * Built in tablespaces are not known by the PersistentTablespace code
	 * so we need to handle them here.
	 */
	if (tblspc == GLOBALTABLESPACE_OID ||
		tblspc == DEFAULTTABLESPACE_OID)
	{
		*master_path = get_filespace_path(SYSTEMFILESPACE_OID, master_dbid);

		if (mirror_dbid)
			*mirror_path = get_filespace_path(SYSTEMFILESPACE_OID,
											  mirror_dbid);

		/* short circuit */
		return;
	}

	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
								tblspc,
							 	master_path,
								mirror_path);
}
