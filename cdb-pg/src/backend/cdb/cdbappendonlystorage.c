/*-------------------------------------------------------------------------
 *
 * cdbappendonlystorage.c
 *
 * Copyright (c) 2007-2009, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "storage/gp_compress.h"
#include "cdb/cdbappendonlystorage_int.h"
#include "cdb/cdbappendonlystorage.h"
#include "utils/pg_crc.h"
#include "utils/guc.h"

static char AOTupleIdBuffer[25];

char* AOTupleIdToString(AOTupleId * aoTupleId)
{
	int segmentFileNum = AOTupleIdGet_segmentFileNum(aoTupleId);
	int64 rowNum = AOTupleIdGet_rowNum(aoTupleId);

	sprintf(AOTupleIdBuffer,"(%d," INT64_FORMAT ")",
		    segmentFileNum, rowNum);

	return AOTupleIdBuffer;
}

