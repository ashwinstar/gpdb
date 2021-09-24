/*-------------------------------------------------------------------------
 *
 * bitmapdesc.c
 *	  rmgr descriptor routines for access/bitmap/bitmapxlog.c
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/bitmapdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bitmap.h"

static void
out_target(StringInfo buf, RelFileNode *node)
{
	appendStringInfo(buf, "rel %u/%u/%u",
			node->spcNode, node->dbNode, node->relNode);
}

void
bitmap_desc(StringInfo buf, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	char		*rec = XLogRecGetData(record);

	switch (info)
	{
		case XLOG_BITMAP_INSERT_LOVITEM:
		{
			xl_bm_lovitem *xlrec = (xl_bm_lovitem *)rec;

			appendStringInfo(buf, "insert a new LOV item: bm_lov_blkno %d", xlrec->bm_lov_blkno);
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_INSERT_META:
		{
			xl_bm_metapage *xlrec = (xl_bm_metapage *)rec;

			appendStringInfo(buf, "update the metapage: ");
			out_target(buf, &(xlrec->bm_node));
			break;
		}

		case XLOG_BITMAP_INSERT_BITMAP_LASTWORDS:
		{
			xl_bm_bitmap_lastwords *xlrec = (xl_bm_bitmap_lastwords *)rec;

			appendStringInfo(buf, "update the last two words in a bitmap: bm_lov_blkno %d bm_lov_offset %d ",
				xlrec->bm_lov_blkno, xlrec->bm_lov_offset);
			out_target(buf, &(xlrec->bm_node));
			break;
		}

		case XLOG_BITMAP_INSERT_WORDS:
		{
			xl_bm_bitmapwords *xlrec = (xl_bm_bitmapwords *)rec;

			appendStringInfo(buf, "insert words in a not-last bitmap page: bm_lov_blkno: %d", xlrec->bm_lov_blkno);
			out_target(buf, &(xlrec->bm_node));
			break;
		}

		case XLOG_BITMAP_UPDATEWORD:
		{
			xl_bm_updateword *xlrec = (xl_bm_updateword *)rec;

			appendStringInfo(buf, "update a word in a bitmap page: bm_blkno %d ", xlrec->bm_blkno);
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		case XLOG_BITMAP_UPDATEWORDS:
		{
			xl_bm_updatewords *xlrec = (xl_bm_updatewords*)rec;

			appendStringInfo(buf, "update words in bitmap pages: bm_lov_blkno %d bm_lov_offset %d bm_new_lastpage %d bm_two_pages %d bm_first_blkno %d bm_second_blkno %d ",
							 xlrec->bm_lov_blkno, xlrec->bm_lov_offset, xlrec->bm_new_lastpage, xlrec->bm_two_pages, xlrec->bm_first_blkno, xlrec->bm_second_blkno);
			out_target(buf, &(xlrec->bm_node));
			break;
		}
		default:
			appendStringInfo(buf, "UNKNOWN");
			break;
	}
}
