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

#include "access/xlog.h"
#include "access/xlogmm.h"
#include "catalog/gp_segment_config.h"
#include "catalog/pg_filespace.h"
#include "catalog/pg_tablespace.h"
#include "cdb/cdbpersistentfilespace.h"
#include "cdb/cdbpersistenttablespace.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "commands/dbcommands.h"
#include "commands/filespace.h"
#include "commands/tablespace.h"
#include "postmaster/postmaster.h"
#include "storage/freespace.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "cdb/cdbpersistentrecovery.h"
#include "cdb/cdbpersistentfilesysobj.h"

typedef struct fspc_map
{
	Oid filespaceoid;
	int16 dbid1;
	int16 dbid2;
	char path1[MAXPGPATH];
	char path2[MAXPGPATH];
} fspc_map;

typedef struct tspc_map
{
	Oid tablespaceoid;
	Oid filespaceoid;
} tspc_map;

/*
 * Container of filespaces/tablespaces used to accumulate mappings
 */
typedef struct fspc_agg_state
{
	union
	{
		int pos;
		int64 dummy;
	};
	fspc_map maps[0]; /* variable length */
} fspc_agg_state;

typedef struct tspc_agg_state
{
	union
	{
		int pos;
		int64 dummy;
	};
	tspc_map maps[0]; /* variable length */
} tspc_agg_state;

static void tblspc_get_filespace_paths(Oid tblspc, int16 master_dbid, 
									   int16 mirror_dbid,
									   char **master_path, char **mirror_path);
static void get_filespace_paths(Oid filespace, char **master_path, char **mirror_path);
static void add_filespace_map_entry(fspc_map *m, char *caller);
static void add_tablespace_map_entry(tspc_map *m, char *caller);

/*
 * Remember filespace and tablespace mappings, used so that we know
 * where to write data to during master mirror synchronisation. Note that
 * segment level replay is different, as since we're not streaming we can be
 * sure to get the persistent tables in shape and just used those.
 */
static HTAB *filespace_map_ht = NULL;
static HTAB *tablespace_map_ht = NULL;

/* extract the object path based on whether we're a mirror or master */
static char *
obj_get_path(xl_mm_fs_obj *xlrec)
{
	char *path = NULL;

	if (xlrec->u.dbid.master == GpIdentity.dbid)
		path = xlrec->master_path;
	else if (xlrec->u.dbid.mirror == GpIdentity.dbid)
		path = xlrec->mirror_path;
	else
	{
		/* 
		 * Not as serious as it looks, must be a recover from before we existed
		 */
		elog(LOG, "mmxlog record not for my dbid %u - [%u, %u]",
			 GpIdentity.dbid, xlrec->u.dbid.master, xlrec->u.dbid.mirror);
	}

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
		elog(DEBUG1, "removing directory, as requested %s", path);
		rmtree(path, true);
	}
	else if (info == MMXLOG_REMOVE_FILE)
	{
		if (Debug_print_qd_mirroring)
			elog(DEBUG1, "unlinking file, as requested %s", path);

		if (unlink(path) < 0)
		{
			if (errno != ENOENT) /* allow it to already be removed */
				elog(WARNING, "could not unlink %s: %m", path);
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
		bool dir_created = true;

		elog(DEBUG1, "got create directory request: %s / %s",
			 xlrec->master_path,
			 xlrec->mirror_path);

		/* 
		 * Inform the persistent table code about the new filespace or
		 * tablespace.
		 */
		if (xlrec->objtype == MM_OBJ_FILESPACE)
		{
			fspc_map m;

			m.filespaceoid = xlrec->filespace;

			m.dbid1 = xlrec->u.dbid.master;
    		strncpy(m.path1, xlrec->master_path, MAXPGPATH);
			
			m.dbid2 = xlrec->u.dbid.mirror;
			strncpy(m.path2, xlrec->mirror_path, MAXPGPATH);
			add_filespace_map_entry(&m, "mmxlog_redo");
		}
		else if (xlrec->objtype == MM_OBJ_TABLESPACE)
		{
			tspc_map m;

			m.filespaceoid = xlrec->filespace;
			m.tablespaceoid = xlrec->tablespace;

			add_tablespace_map_entry(&m, "mmxlog_redo");
		}

		if (mkdir(path, 0700) < 0)
		{
			/* allowed to already exist */
			if (errno != EEXIST)
			{
				dir_created = false;
				elog(WARNING, "could not create directory %s: %m", path);
			}
		}

		/* need to add PG_VERSION for newly created databases */
		if (xlrec->objtype == MM_OBJ_DATABASE && dir_created == true)
			set_short_version(path, NULL, false);
	}
	else if (info == MMXLOG_REMOVE_DIR)
	{
		if (xlrec->objtype == MM_OBJ_DATABASE)
			xlog_prepare_database_drop(xlrec->tablespace, xlrec->database);

		/* tablespace and database should be fine */
		unlink_obj(path, info);
	}
	else if (info == MMXLOG_CREATE_FILE)
	{
		int fd = open(path, O_CREAT | O_RDWR | PG_BINARY, 0600);

		if (fd < 0)
		{
			/* tolerate existence */
			if (errno != EEXIST)
				elog(WARNING, "could open open file %s: %m", path);
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
		RelFileNode rnode;

		Insist(xlrec->objtype == MM_OBJ_RELFILENODE);

		rnode.spcNode = xlrec->tablespace;
		rnode.dbNode = xlrec->database;
		rnode.relNode = xlrec->relfilenode;

		DropRelFileNodeBuffers(rnode,
							   false /* not a local buffer if we got here*/,
							   0); 
		FreeSpaceMapForgetRel(&rnode);

		XLogDropRelation(rnode);

		if (Debug_print_qd_mirroring)
			elog(DEBUG1, "dropped buffers and freespace for %u/%u/%u",
				 rnode.spcNode, rnode.dbNode, rnode.relNode);
		unlink_obj(path, info);
	}
	else
		elog(PANIC, "unknown mmxlog op code %u", info);
}

/*
 * For log output
 */
void
mmxlog_desc(StringInfo buf, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	char		*rec = XLogRecGetData(record);
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
static bool
emit_mmxlog_fs_record(mm_fs_obj_type type, Oid filespace,
					  Oid tablespace, Oid database, Oid relfilenode,
					  uint32 segnum, uint8 flags, XLogRecPtr *location)
{
	MemSet(location, 0, sizeof(XLogRecPtr));

	if (!CanEmitXLogRecords)
		/* no business here */
		return false;

	/* only interesting on the master */
	if (GpIdentity.dbid == MASTER_DBID)
	{
		XLogRecData rdata;
		xl_mm_fs_obj xlrec;
		char *master_path;
		char *mirror_path;
		int16 master_dbid = GpIdentity.dbid;
		int16 mirror_dbid = GpStandbyDbid;
		
		Insist(Gp_role == GP_ROLE_DISPATCH ||
			   Gp_role == GP_ROLE_UTILITY);

		master_path = NULL;
		mirror_path = NULL;

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

		Insist(!master_path ||
			   strlen(master_path) <= MAXPGPATH);

		xlrec.master_path[0] = '\0';
		if (master_path)
			strncpy(xlrec.master_path, master_path,
					sizeof(xlrec.master_path));
		else
		{
			/* 
			 * Allow relative paths if we didn't get anything when we looked up
			 * the filespace. We must allow this for the default filespace.
			 */
			xlrec.master_path[0] = '.';
			xlrec.master_path[1] = '\0';
		}

		append_file_parts(type, xlrec.master_path, tablespace, database,
						  relfilenode, segnum);

		xlrec.mirror_path[0] = '\0';
		if (mirror_path)
			strncpy(xlrec.mirror_path, mirror_path,
					sizeof(xlrec.mirror_path));
		else
		{
			xlrec.mirror_path[0] = '.';
			xlrec.mirror_path[1] = '\0';
		}

		append_file_parts(type, xlrec.mirror_path, tablespace, database,
						  relfilenode, segnum);
		
		if (Debug_print_qd_mirroring)
			elog(DEBUG1, "XLOG: type = %i, flags = %i, dbid = (%u, %u), path = (%s, %s)",
				 type, flags, xlrec.u.dbid.master, xlrec.u.dbid.mirror,
				 xlrec.master_path, xlrec.mirror_path);

		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;
		
		*location = XLogInsert(RM_MMXLOG_ID, flags | XLOG_NO_TRAN, &rdata);

		return true;
	}
	else
		return false;
}

/* External interface to filespace removal logging */
void
mmxlog_log_remove_filespace(Oid filespace)
{
	bool emitted;
	XLogRecPtr location;

	if (Debug_print_qd_mirroring)
		elog(DEBUG1, "emitting drop filespace record for %u",
			 filespace);
	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_FILESPACE,
						  filespace,
						  InvalidOid /* tablespace */,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_REMOVE_DIR,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_remove_filespace: delete filespace %u (emitted %s, location %s)",
			 filespace,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/* External interface to tablespace removal logging */
void
mmxlog_log_remove_tablespace(Oid tablespace)
{
	bool emitted;
	XLogRecPtr location;

	if (Debug_print_qd_mirroring)
		elog(DEBUG1, "emitting drop tablespace record for %u",
			 tablespace);
	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_TABLESPACE,
						  InvalidOid /* filespace */,
						  tablespace,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_REMOVE_DIR,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_remove_tablespace: delete tablespace %u (emitted %s, location %s)",
			 tablespace,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/* External interface to database removal logging */
void
mmxlog_log_remove_database(Oid tablespace, Oid database)
{
	bool emitted;
	XLogRecPtr location;

	if (Debug_print_qd_mirroring)
		elog(DEBUG1, "emitting drop database record for %u/%u",
			 tablespace, database);
	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_DATABASE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_REMOVE_DIR,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_remove_database: delete database directory %u/%u (emitted %s, location %s)",
			 tablespace,
			 database,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/* External interface to relfilenode removal logging */
void
mmxlog_log_remove_relfilenode(Oid tablespace, Oid database, Oid relfilenode,
							  uint32 segnum)
{
	bool emitted;
	XLogRecPtr location;

	if (Debug_print_qd_mirroring)
		elog(DEBUG1, "emitting drop relfilenode record for %u/%u/%u",
			 tablespace, database, relfilenode);
	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_RELFILENODE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  relfilenode,
						  segnum,
						  MMXLOG_REMOVE_FILE,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_remove_relfilenode: delete relation %u/%u/%u, segment file #%d (emitted %s, location %s)",
			 tablespace,
			 database,
			 relfilenode,
			 segnum,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/* External interface to filespace creation logging */
void
mmxlog_log_create_filespace(Oid filespace)
{
	bool emitted;
	XLogRecPtr location;

	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_FILESPACE,
						  filespace,
						  InvalidOid /* tablespace */,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_CREATE_DIR,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_create_filespace: create filespace %u (emitted %s, location %s)",
			 filespace,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/* External interface to tablespace creation logging */
void
mmxlog_log_create_tablespace(Oid filespace, Oid tablespace)
{
	bool emitted;
	XLogRecPtr location;

	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_TABLESPACE,
						  filespace,
						  tablespace,
						  InvalidOid /* database */,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_CREATE_DIR,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_create_tablespace: create tablespace %u (filespace %u, emitted %s, location %s)",
			 tablespace,
			 filespace,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/* External interface to database creation logging */
void
mmxlog_log_create_database(Oid tablespace, Oid database)
{
	bool emitted;
	XLogRecPtr location;

	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_DATABASE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  InvalidOid /* relfilenode */,
						  0 /* segnum */,
						  MMXLOG_CREATE_DIR,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_create_database: create database directory %u/%u (emitted %s, location %s)",
			 tablespace,
			 database,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/* External interface to relfilenode creation logging */
void
mmxlog_log_create_relfilenode(Oid tablespace, Oid database,
							  Oid relfilenode, uint32 segnum)
{
	bool emitted;
	XLogRecPtr location;

	emitted =
		emit_mmxlog_fs_record(
						  MM_OBJ_RELFILENODE,
						  InvalidOid /* filespace */,
						  tablespace,
						  database,
						  relfilenode,
						  segnum,
						  MMXLOG_CREATE_FILE,
						  &location);
	if (Debug_persistent_recovery_print)
	{
		SUPPRESS_ERRCONTEXT_DECLARE;

		SUPPRESS_ERRCONTEXT_PUSH();

		elog(PersistentRecovery_DebugPrintLevel(), 
			 "mmxlog_log_create_relfilenode: create relation %u/%u/%u, segment file #%d (emitted %s, location %s)",
			 tablespace,
			 database,
			 relfilenode,
			 segnum,
			 (emitted ? "true" : "false"),
			 XLogLocationToString(&location));

		SUPPRESS_ERRCONTEXT_POP();
	}
}

/*
 * We need to make sure the memory for these maps doesn't go away.
 */
static void *
map_alloc(Size sz)
{
	/* XXX: might have to allocate in a higher memory context */
	return palloc(sz);
}

/*
 * Generic initialisation of hash table.
 */
static HTAB *
init_hash(const char *name, Size keysize, Size entrysize)
{
	HASHCTL ctl;

	ctl.keysize = keysize;
	ctl.entrysize = entrysize;
	ctl.alloc = map_alloc;
	return hash_create(name,
					   gp_max_filespaces,
					   &ctl,
					   HASH_ELEM | HASH_ALLOC);
}

/*
 * Add a new mapping to the filespace hash table. We do not support the
 * complementary filespace mapping removal function because we do not want to
 * get into situations where we've removed a filespace but still data to apply
 * to some file in the filespace. Unfortunately, WAL needs to just do what it is
 * told and it could be told to do this if we get the logic wrong on the other
 * end.
 */
static void
add_filespace_map_entry(fspc_map *m, char *caller)
{
	void *entry;
	bool found;

	/* 
	 * The table is lazily initialised.
	 */
	if (!filespace_map_ht)
		filespace_map_ht = init_hash("mmxlog filespace map",
									 sizeof(Oid), /* keysize */
									 sizeof(fspc_map));

	entry = hash_search(filespace_map_ht,
						&(m->filespaceoid),
						HASH_ENTER,
						&found);
	/*
	 * If this is a new entry, we need to add the data, if we found 
	 * an entry, we need to update it, so just copy our data 
	 * right over the top.
	 */
	memcpy(entry, m, sizeof(fspc_map));

	if (Debug_persistent_recovery_print)
	{
		elog(PersistentRecovery_DebugPrintLevel(), 
			 "add_filespace_map_entry: add filespace %u, dbid1 %d, dbid2 %d (caller '%s')",
			 m->filespaceoid,
			 m->dbid1,
			 m->dbid2,
			 caller);
	}
}

/*
 * Same as add_filespace_map_entry(), but for tablespaces.
 */
static void
add_tablespace_map_entry(tspc_map *m, char *caller)
{
	void *entry;
	bool found;

	/* 
	 * The table is lazily initialised.
	 */
	if (!tablespace_map_ht)
		tablespace_map_ht = init_hash("mmxlog tablespace map",
									 sizeof(Oid), /* keysize */
									 sizeof(tspc_map));

	entry = hash_search(tablespace_map_ht,
						&(m->tablespaceoid),
						HASH_ENTER,
						&found);
	/*
	 * See above for why we do this.
	 */
	memcpy(entry, m, sizeof(tspc_map));

	if (Debug_persistent_recovery_print)
	{
		elog(PersistentRecovery_DebugPrintLevel(), 
			 "add_tablespace_map_entry: add tablespace %u (filespace %u, caller '%s')",
			 m->tablespaceoid,
			 m->filespaceoid,
			 caller);
	}
}

/*
 * Given a filespace oid, return that path to the filespace.
 */
char *
mmxlog_filespace_get_path(Oid fspcoid)
{
	bool found;
	fspc_map *m;
	char *path = NULL;

	Insist(fspcoid != SYSTEMFILESPACE_OID);

	/* 
	 * The table is lazily initialised.
	 */
	if (!filespace_map_ht)
		filespace_map_ht = init_hash("mmxlog filespace map",
									 sizeof(Oid), /* keysize */
									 sizeof(fspc_map));

	m = hash_search(filespace_map_ht,
					&fspcoid,
					HASH_FIND,
					&found);

	if (!found)
		elog(ERROR, "cannot find filespace path for filespace OID %u",
			 fspcoid);

	if (GPStandby())
		path = m->path2;
	else
	{
		Insist(m->dbid1 == GpIdentity.dbid);
		path = m->path1;
	}

	elog(DEBUG1, "MMXLOG: looking for fspc with oid %u, returning %s",
		 fspcoid, path);
	return path;
}

/*
 * The same as mmxlog_filespace_get_path() but for tablespaces.
 */
char *
mmxlog_tablespace_get_path(Oid tspcoid)
{
	Oid fspcoid;
	tspc_map *m;
	bool found;

	elog(DEBUG1, "MMXLOG: looking for tspcoid %u", tspcoid);

	/* 
	 * The table is lazily initialised.
	 */
	if (!tablespace_map_ht)
		tablespace_map_ht = init_hash("mmxlog tablespace map",
									 sizeof(Oid), /* keysize */
									 sizeof(tspc_map));

	/*
	 * First, get the filespace that the tablespace resides in.
	 */
	m = hash_search(tablespace_map_ht, &tspcoid, HASH_FIND, &found);

	if (!found)
		elog(ERROR, "cannot find tablespace map for tablespace %u",
			 tspcoid);

	fspcoid = m->filespaceoid;

	return mmxlog_filespace_get_path(fspcoid);
}

static void
agg_fspc(void *s, Oid filespace, int16 dbid1, char *path1,
		 int16 dbid2, char *path2)
{
	fspc_map m;
	fspc_agg_state *fas = (fspc_agg_state *)s;

	Insist(fas->pos < gp_max_filespaces);

	m = fas->maps[fas->pos++];
	m.filespaceoid = filespace;

	m.dbid1 = dbid1;
	strncpy(m.path1, path1, MAXPGPATH);

	m.dbid2 = dbid2;
	strncpy(m.path2, path2, MAXPGPATH);
}

static void
agg_tspc(void *s, Oid filespace, Oid tablespace)
{
	tspc_map m;
	tspc_agg_state *tas = (tspc_agg_state *)s;

	m = tas->maps[tas->pos++];
	m.filespaceoid = filespace;
	m.tablespaceoid = tablespace;
}

/*
 * Add filespace and tablespace OID => path maps to the checkpoint payload. This
 * is used by the standby to construct a valid picture of the filespace and
 * tablespace configuration without having to touch the persistent tables --
 * which it cannot do since they're not guaranteed to be in a consistent state.
 */
void
mmxlog_append_checkpoint_data(XLogRecData rdata[4])
{
	fspc_agg_state *f;
	tspc_agg_state *t;
	Size sz_f;
	Size sz_t;

	/* 
	 * We must make sure no one traverses the rdata chain into uninitialised
	 * data if we exit early, below.
	 */
	rdata[1].next = NULL;
	rdata[2].next = NULL;
	rdata[3].next = NULL;

	if (gp_before_filespace_setup)
		return;

	if (GpIdentity.dbid != MASTER_DBID)
		return;

	sz_f = sizeof(fspc_agg_state) + (sizeof(fspc_map) * gp_max_filespaces);
	sz_t = sizeof(tspc_agg_state) + (sizeof(tspc_map) * gp_max_tablespaces);
	f = palloc0(sz_f);
	t = palloc0(sz_t);

	agg_filespace_data((void *)f, agg_fspc);
	agg_tablespace_data((void  *)t, agg_tspc);

	rdata[2].data = (void *)f->maps;
	rdata[2].buffer = InvalidBuffer;
	rdata[2].len = sz_f - sizeof(fspc_agg_state);
	rdata[3].data = (void *)t->maps;
	rdata[3].buffer = InvalidBuffer;
	rdata[3].len = sz_t - sizeof(tspc_agg_state);

	rdata[1].next = &(rdata[2]);
	rdata[2].next = &(rdata[3]);
}

/* 
 * If we're on the master standby, we expect to receive filespace and tablespace
 * meta data from a checkpoint.
 */
void
mmxlog_read_checkpoint_data(char *cpdata)
{
	fspc_map *fmap = NULL;
	tspc_map *tmap = NULL;
	int n;
	int i;

	if (!cpdata)
		return;

	if (!GPStandby())
		return;
 	fmap = (fspc_map *)cpdata;
	tmap = (tspc_map *)(cpdata + (sizeof(fspc_map) * gp_max_filespaces));

	/*
	 * Push the data down into the hash tables. We calculate the array length
	 * from the byte length of the array. We need to do filespaces first as
	 * they are the root of the space hierarchy.
	 */
	n = gp_max_filespaces;

	for (i = 0; i < n; i++)
	{
		fspc_map *m = &(fmap[i]);

		if (!m)
			break;

		add_filespace_map_entry(m, "mmxlog_read_checkpoint_data");
		
		if (mkdir(fmap[i].path2, 0700) < 0)
		{
			/* allowed to already exist */
			if (errno != EEXIST)
				elog(WARNING, "could not create directory %s: %m",
					 fmap[i].path2);
		}
	}

	n = gp_max_tablespaces;
	for (i = 0; i < n; i++)
	{
		char path[MAXPGPATH];
		char *tmp;
		tspc_map *m = &(tmap[i]);

		add_tablespace_map_entry(m, "mmxlog_read_checkpoint_data");

		tmp = mmxlog_filespace_get_path(tmap[i].filespaceoid);
		snprintf(path, sizeof(path), "%s/%u", tmp, tmap[i].tablespaceoid);

		if (mkdir(path, 0700) < 0)
		{
			/* allowed to already exist */
			if (errno != EEXIST)
				elog(WARNING, "could not create directory %s: %m", path);
		}
	}
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
		*master_path = NULL;

		if (mirror_dbid)
			*mirror_path = NULL;

		/* short circuit */
		return;
	}

	PersistentTablespace_GetPrimaryAndMirrorFilespaces(
								tblspc,
							 	master_path,
								mirror_path);
}
