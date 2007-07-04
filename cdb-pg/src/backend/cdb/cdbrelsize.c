/*-------------------------------------------------------------------------
 *
 * cdbrelsize.h
 *
 * Get the max size of the relation across the segDBs
 *
 * Copyright (c) 2006, greenplum inc
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "catalog/catalog.h"
#include "cdb/cdbvars.h"
#include "miscadmin.h"
#include "cdb/cdbdisp.h"
#include "libpq-fe.h"
#include "lib/stringinfo.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"

#include "cdb/cdbrelsize.h"

int64 cdbRelSize(Oid relOid)
{

	int64	size = 0;
	int		i;
	int 	resultCount = 0;
	struct pg_result **results = NULL;
	
	StringInfoData errbuf;
	StringInfoData buffer;
	/*
	 * Let's ask the QEs for the size of the relation
	 */
	initStringInfo(&buffer);
	initStringInfo(&errbuf);
	
	/* 
	 * Safer to pass names than oids, just in case they get out of sync between QD and QE,
	 * which might happen with a toast table or index, I think (but maybe not)
	 */
	appendStringInfo(&buffer, "select pg_relation_size('%s.%s')", quote_identifier(get_namespace_name(get_rel_namespace(relOid))),quote_identifier(get_rel_name(relOid)));

	/* 
	 * In the future, it would be better to send the command to only one QE for the optimizer's needs,
	 * but for ALTER TABLE, we need to be sure if the table has any rows at all.
	 */
	results = cdbdisp_dispatchRMCommand(buffer.data, true, false, &errbuf, &resultCount);

	if (errbuf.len > 0)
	{
		ereport(WARNING, (errmsg("cdbRelSize error (gathered %d results from cmd '%s')", resultCount, buffer.data),
						errdetail("%s", errbuf.data)));
		pfree(errbuf.data);
		pfree(buffer.data);
		
		return -1;
	}
	else
	{
										
		for (i = 0; i < resultCount; i++)
		{
			if (PQresultStatus(results[i]) != PGRES_TUPLES_OK)
			{
				elog(ERROR,"cdbRelSize: resultStatus not tuples_Ok: %s   %s",PQresStatus(PQresultStatus(results[i])),PQresultErrorMessage(results[i]));
			}
			else
			{
				int64 tempsize = 0;
				(void) scanint8(PQgetvalue(results[i], 0, 0), false, &tempsize);
				if (tempsize > size)
				 	size = tempsize;
			}
		}
	
		pfree(errbuf.data);
		pfree(buffer.data);

		for (i = 0; i < resultCount; i++)
			PQclear(results[i]);
	
		free(results);
	}
	
	return size;

}
