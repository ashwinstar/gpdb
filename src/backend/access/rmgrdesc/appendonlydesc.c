/*-------------------------------------------------------------------------
 *
 * appendonlydesc.c
 *	  rmgr descriptor routines for cdb/cdbappendonlystorage.c
 *
 * Portions Copyright (c) 2007-2009, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 *
 *
 * IDENTIFICATION
 *	    src/backend/access/rmgrdesc/appendonlydesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/cdbappendonlystorage_int.h"
#include "cdb/cdbappendonlystorage.h"
#include "cdb/cdbappendonlyxlog.h"

void
appendonly_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		  info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_APPENDONLY_INSERT:
			{
				xl_ao_insert *xlrec = (xl_ao_insert *)rec;

				appendStringInfo(
					buf,
					"insert: rel %u/%u/%u seg/offset:%u/" INT64_FORMAT " len:%lu",
					xlrec->target.node.spcNode, xlrec->target.node.dbNode,
					xlrec->target.node.relNode, xlrec->target.segment_filenum,
					xlrec->target.offset, 0);
			}
			break;
		case XLOG_APPENDONLY_TRUNCATE:
			{
				xl_ao_truncate *xlrec = (xl_ao_truncate*)rec;

				appendStringInfo(
					buf,
					"truncate: rel %u/%u/%u seg/offset:%u/" INT64_FORMAT,
					xlrec->target.node.spcNode, xlrec->target.node.dbNode,
					xlrec->target.node.relNode, xlrec->target.segment_filenum,
					xlrec->target.offset);
			}
			break;
		default:
			appendStringInfo(buf, "UNKNOWN");
	}
}
