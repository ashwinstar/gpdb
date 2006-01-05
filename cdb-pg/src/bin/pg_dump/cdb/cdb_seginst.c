/*-------------------------------------------------------------------------
 *
 * cdb_seginst.c
 *
 * Structures and functions to read a join of cdb_seg, cdb_instance, and 
 * mpp_segment_instance tables using libpq
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "pqexpbuffer.h"
#include "libpq-fe.h"
#include <regex.h>
#include <sys/stat.h>
#include <limits.h>
#include <assert.h>
#include "cdb_dump_util.h"
#include "cdb_table.h"
#include "cdb_seginst.h"

/* FreeSegmentDatabaseArray: This function frees the memory allocated for ther pSegDBAr->pData array,
 * and the strings that were allocated for each element in the array
 * It does NOT free the pSegDBAr itself.
 */
void FreeSegmentDatabaseArray( SegmentDatabaseArray* pSegDBAr )
{
	int i;
	if ( pSegDBAr == NULL )
		return;
	
	for ( i=0; i<pSegDBAr->count; i++ )
	{
		SegmentDatabase* pSegDB = &pSegDBAr->pData[i];
			
		if ( pSegDB->pszHost != NULL )
			free( pSegDB->pszHost );
        /*
		if ( pSegDB->pszDBName != NULL )
			free( pSegDB->pszDBName );
         */
	}

	if ( pSegDBAr->pData != NULL )
		free( pSegDBAr->pData );
	
	pSegDBAr->count = 0;
	pSegDBAr->pData = NULL;
}

void FreeRestorePairArray( RestorePairArray* restorePairAr )
{
	int i;
	if ( restorePairAr == NULL )
		return;
	
//	for ( i = 0; i < restorePairAr->count; i++ )
//	{
//		SegmentDatabase* tSegDB = &restorePairAr->pData[i].segdb_target;
//		SegmentDatabase* sSegDB = &restorePairAr->pData[i].segdb_source;
//	}
	
	if ( restorePairAr->pData != NULL )
		free( restorePairAr->pData );
	
	restorePairAr->count = 0;
	restorePairAr->pData = NULL;
}

/* GetSegmentDatabaseArray: This function reads all active instance segment pairs 
 * from the function mpp_segment_instance_map().
 * It then uses the set specification fo filter out any instid, segid combinations
 * that don't match the set.  bExcludeHead = true is used for restoring, because
 * the head is restored first seperately from the rest of the databases. 
 /
*/

bool GetDumpSegmentDatabaseArray(PGconn* pConn, 
								 SegmentDatabaseArray* pSegDBAr, 
								 ActorSet actors, 
								 bool dataOnly,
								 bool schemaOnly)
{
	bool		bRtn = true;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;
	int ntups;
	int count;
	int i_dbid;
	int i_role;
    int i_isvalid;
    int i_isprimary;
	int i_host;
	int i_port;
	int i;
	int x;
	SegmentDatabase* pSegDB;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry,  "SELECT" 
							   " dbid," 
							   " content,"  
							   " valid,"
							   " isPrimary,"
							   " hostname,"
							   " port"
							 " FROM"
							   " mpp_configuration"
							 " WHERE valid = 't'");

	pRes = PQexec(pConn, pQry->data);
	if (!pRes || PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		mpp_err_msg("ERROR", "mpp_dump","query to obtain list of MPP segment databases failed: %s",
				  PQerrorMessage(pConn));
		bRtn = false;
		goto cleanup;		  
	}
	
	ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		mpp_err_msg("ERROR", "mpp_dump","no MPP segment databases found on master segment schema");
		bRtn = false;
		goto cleanup;		  
	}
	
	count = ntups + 1;
	// See how many set elements there are that were specified by HostIP or name.
	
	/* Allocate enough memory for all of them, even though some may be filtered out.  */
	pSegDBAr->count = 0;
	pSegDBAr->pData = (SegmentDatabase *) calloc( count,  sizeof(SegmentDatabase) );
	if ( pSegDBAr->pData == NULL )
	{
		mpp_err_msg("ERROR", "mpp_dump","Unable to allocate memory for MPP segment/instances information\n");
		bRtn = false;
		goto cleanup;		  
	}

	/* get the column numbers */
	i_dbid		= PQfnumber(pRes, "dbid");
	i_role		= PQfnumber(pRes, "content");
	i_isvalid	= PQfnumber(pRes, "valid");
	i_host		= PQfnumber(pRes, "hostname");
	i_port		= PQfnumber(pRes, "port");
    i_isprimary = PQfnumber(pRes, "isPrimary");

	/* Read through the results set.  */
	x = 0;
	for ( i = 0; i < ntups; i++ )
	{
		int dbid	= atoi(PQgetvalue(pRes, i, i_dbid));
		int roleid	= atoi(PQgetvalue(pRes, i, i_role));
		int isprimary = (strcmp(PQgetvalue(pRes, i, i_isprimary), "t") == 0);
         
		/* if we want primaries only - skip all mirrors */
		if (actors == SET_NO_MIRRORS && !isprimary)
           continue;
		
		/* in dataOnly we don't dump master information */
		if(dataOnly && roleid == -1)
			continue;
		
		/* in schemaOnly we skip all but the master */
		if(schemaOnly && roleid != -1)
			continue;
			
		pSegDB = &pSegDBAr->pData[x++];
		pSegDB->dbid		= dbid; 
		pSegDB->role   = (roleid == -1 ? ROLE_MASTER : ROLE_SEGDB);
		pSegDB->port		= atoi(PQgetvalue(pRes, i, i_port));
		pSegDB->isValid	= ( strcmp(PQgetvalue(pRes, i, i_isvalid), "t") == 0 );
		pSegDB->pszHost	= strdup(PQgetvalue(pRes, i, i_host));
        pSegDB->pszDBName = PQdb(pConn);
		pSegDB->pszDBUser = PQuser(pConn);
	}
	
	/* set the count to be the number that passed the set inclusion test */
	pSegDBAr->count = x;
	
cleanup:
	//FreeCDBSegArray( &SegAr );
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);
	if ( pRes != NULL )
		PQclear(pRes);

	return bRtn;
}

bool GetRestoreSegmentDatabaseArray(PGconn* pConn, 
							 RestorePairArray *restorePairAr,
							 BackupLoc	backupLocation,
							 bool dataOnly)
{
	bool		bRtn = true;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;
	int ntups;
	int count;
	int i_dbid;
	int i_content;
    int i_isvalid;
    int i_isprimary;
	int i_host;
	int i_port;
	int i;
	int x;
	SegmentDatabase* sourceSegDB;
	SegmentDatabase* targetSegDB;
	SegmentDatabase* prevSourceSegDB;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry,  "SELECT" 
					  " dbid," 
					  " content,"  
					  " valid,"
					  " isPrimary,"
					  " hostname,"
					  " port"
					  " FROM"
					  " mpp_configuration"
					  " WHERE valid = 't'" 
					  " ORDER BY content, isPrimary DESC"
					  );
	
	pRes = PQexec(pConn, pQry->data);
	if (!pRes || PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		mpp_err_msg("ERROR", "mpp_restore","query to obtain list of MPP segment databases failed: %s",
					  PQerrorMessage(pConn));
		bRtn = false;
		goto cleanup;		  
	}
	
	ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		mpp_err_msg("ERROR", "mpp_restore","no MPP segment databases found on master segment schema");
		bRtn = false;
		goto cleanup;		  
	}
	
	count = ntups + 1;
	// See how many set elements there are that were specified by HostIP or name.
	
	/* Allocate enough memory for all of them, even though some may be filtered out.  */
	
	restorePairAr->count = 0;
	restorePairAr->pData = (RestorePair *) calloc( count,  sizeof(RestorePair) );
	if ( restorePairAr->pData == NULL )
	{
		mpp_err_msg("ERROR", "mpp_restore","Unable to allocate memory for MPP segment database information\n");
		bRtn = false;
		goto cleanup;		  
	}
	
	/* get the column numbers */
	i_dbid		= PQfnumber(pRes, "dbid");
	i_content	= PQfnumber(pRes, "content");
	i_isvalid	= PQfnumber(pRes, "valid");
	i_host		= PQfnumber(pRes, "hostname");
	i_port		= PQfnumber(pRes, "port");
    i_isprimary = PQfnumber(pRes, "isPrimary");
	
	/* Read through the results set.  */
	x = 0;
	for ( i = 0; i < ntups; i++ )
	{
		int dbid		= atoi(PQgetvalue(pRes, i, i_dbid));
		int contentid	= atoi(PQgetvalue(pRes, i, i_content));
		int isprimary	= (strcmp(PQgetvalue(pRes, i, i_isprimary), "t") == 0);
		
		/* if dataOnly don't restore the master (table definitions) */
		if(dataOnly && contentid == -1)
			continue;
		
		/* we restore all segDBs, so mark this one as a target */
		targetSegDB = &restorePairAr->pData[x].segdb_target; 
		targetSegDB->dbid		= dbid; 
		targetSegDB->role		= (contentid == -1 ? ROLE_MASTER : ROLE_SEGDB);
		targetSegDB->port		= atoi(PQgetvalue(pRes, i, i_port));
		targetSegDB->isValid	= ( strcmp(PQgetvalue(pRes, i, i_isvalid), "t") == 0 );
		targetSegDB->pszHost	= strdup(PQgetvalue(pRes, i, i_host));
        targetSegDB->pszDBName = PQdb(pConn);
		targetSegDB->pszDBUser = PQuser(pConn);
		
		sourceSegDB = &restorePairAr->pData[x].segdb_source;
		
		if(backupLocation == FILE_ON_PRIMARIES && !isprimary)
		{
			/* 
			 * if this is a mirror, and backup files are only on the 
			 * primary nodes, make the primary for this content (previous
			 * tuple in the result set) the source for this seg db backup.
			 */
			prevSourceSegDB = &restorePairAr->pData[x - 1].segdb_source;
			sourceSegDB->dbid		= prevSourceSegDB->dbid;  
			sourceSegDB->role		= prevSourceSegDB->role;
			sourceSegDB->port		= prevSourceSegDB->port;
			sourceSegDB->isValid	= prevSourceSegDB->isValid;
			sourceSegDB->pszHost	= prevSourceSegDB->pszHost;
			sourceSegDB->pszDBName  = prevSourceSegDB->pszDBName;
			sourceSegDB->pszDBUser  = prevSourceSegDB->pszDBUser;		
		}
		else
		{
			/* 
			 * the source is the same as the target segDB. Backup file
			 * is on the segdb we want to backup. 
			 */
			sourceSegDB->dbid		= targetSegDB->dbid;  
			sourceSegDB->role		= targetSegDB->role;
			sourceSegDB->port		= targetSegDB->port;
			sourceSegDB->isValid	= targetSegDB->isValid;
			sourceSegDB->pszHost	= targetSegDB->pszHost;
			sourceSegDB->pszDBName  = targetSegDB->pszDBName;
			sourceSegDB->pszDBUser  = targetSegDB->pszDBUser;
		}
		
		x++;

	}
	
	/* set the count to be the number that passed the set inclusion test */
	restorePairAr->count = x;
	
cleanup:
		//FreeCDBSegArray( &SegAr );
		if ( pQry != NULL )
			destroyPQExpBuffer(pQry);
	if ( pRes != NULL )
		PQclear(pRes);
	
	return bRtn;
}

/* cmpInstSeg: This function compares two InstSeg objects.  
 * The sorting is by instType, instVal, segType, segVal 
 */
/*
int cmpInstSeg( const void* l, const void* r )
{
	int cmp;
	InstSeg* lhs = (InstSeg*) l;
	InstSeg* rhs = (InstSeg*) r;
	
	// Sort based on instType, instVal, segType, segVal
	if ( lhs->instVal.val_type < rhs->instVal.val_type )
		return -1;
	if ( lhs->instVal.val_type > rhs->instVal.val_type )
		return 1;
	
	switch( lhs->instVal.val_type )
 	{
		case DEFN_TYPE_ID:
			if ( lhs->instVal.val_id < rhs->instVal.val_id )
				return -1;
			if ( lhs->instVal.val_id > rhs->instVal.val_id )
				return 1;
			break;
		case DEFN_TYPE_ALL:
			break;
		case DEFN_TYPE_HOST:
			assert( lhs->instVal.val_pszHost != NULL && rhs->instVal.val_pszHost != NULL );
			cmp = strcmp( lhs->instVal.val_pszHost, rhs->instVal.val_pszHost );
			if ( cmp != 0 )
				return cmp;
			break;
		default:
			assert(0);
			return 0;
	}
	
	if ( lhs->segVal.val_type < rhs->segVal.val_type )
		return -1;
	if ( lhs->segVal.val_type > rhs->segVal.val_type )
		return 1;
	
	switch( lhs->segVal.val_type )
	{
		case DEFN_TYPE_ID:
			if ( lhs->segVal.val_id < rhs->segVal.val_id )
				return -1;
			if ( lhs->segVal.val_id > rhs->segVal.val_id )
				return 1;
			break;
		case DEFN_TYPE_ALL:
			break;
		default:
			assert(0);
			return 0;
	}
	
	return 0;
}
*/


