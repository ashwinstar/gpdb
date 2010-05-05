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

extern void mmxlog_redo(XLogRecPtr lsn, XLogRecord *record);
extern void mmxlog_desc(StringInfo buf, uint8 xl_info, char *rec);

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
extern void mmxlog_log_create_tablespace(Oid tablespace);
extern void mmxlog_log_create_database(Oid tablespace, Oid database);
extern void mmxlog_log_create_relfilenode(Oid tablespace, Oid database,
										  Oid relfilenode, uint32 segnum);

#endif /* MMXLOG_H */
