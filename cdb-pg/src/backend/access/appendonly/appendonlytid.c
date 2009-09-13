/*-------------------------------------------------------------------------
 *
 * appendonlytid.c
 *
 * Copyright (c) 2007-2009, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/appendonlytid.h"

static char AOTupleIdBuffer[25];

char* AOTupleIdToString(AOTupleId * aoTupleId)
{
	int segmentFileNum = AOTupleIdGet_segmentFileNum(aoTupleId);
	int64 rowNum = AOTupleIdGet_rowNum(aoTupleId);

	sprintf(AOTupleIdBuffer,"(%d," INT64_FORMAT ")",
		    segmentFileNum, rowNum);

	return AOTupleIdBuffer;
}
