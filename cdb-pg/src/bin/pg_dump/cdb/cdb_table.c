/*-------------------------------------------------------------------------
 *
 * cdb_table.c
 *
 * Structures and functions to read cdb tables using libpq.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "pqexpbuffer.h"
#include "libpq-fe.h"
#include "cdb_dump_util.h"
#include "cdb_table.h"

/*-------------------------------------------------------------------------
 *
 * FreeCDBAssignmentArray( CDBAssignmentArray* pAssignmentAr ) returns void
 *
 * This frees the pointers allocated in each of the CDBAssignment structs
 * that are part of the pAssignmentAr->pData array, and then frees the pData 
 * pointer itself.  It does NOT free the pAssignmentAr.
 *-------------------------------------------------------------------------
 */
/*
void FreeCDBAssignmentArray( CDBAssignmentArray* pAssignmentAr )
{
	if ( pAssignmentAr == NULL )
		return;
	
	int i;
	for ( i=0; i<pAssignmentAr->count; i++ )
	{
		CDBAssignment* pAssignment = &pAssignmentAr->pData[i]; 
			
		if ( pAssignment->pszTableName != NULL )
			free( pAssignment->pszTableName );
		if ( pAssignment->pszNspName != NULL )
			free( pAssignment->pszNspName );
	}
	
	if ( pAssignmentAr->pData != NULL )
	{
		free( pAssignmentAr->pData );
		pAssignmentAr->pData = NULL;
	}
	
	pAssignmentAr->count = 0;
}
*/

/*-------------------------------------------------------------------------
 *
 * FreeCDBBaseTableArray( CDBBaseTableArray* pTableAr ) returns void
 *
 * This frees the pointers allocated in each of the CBBBaseTable structs
 * that are part of the pTableAr->pData array, and then frees the pData 
 * pointer itself.  It does NOT free the pTableAr.
 *-------------------------------------------------------------------------
 */

void FreeCDBBaseTableArray( CDBBaseTableArray* pTableAr )
{
	if ( pTableAr == NULL )
		return;
	
	int i;
	for ( i=0; i<pTableAr->count; i++ )
	{
		CDBBaseTable* pBaseTable = &pTableAr->pData[i]; 
			
		if ( pBaseTable->pszTableName != NULL )
			free( pBaseTable->pszTableName );
		if ( pBaseTable->pszNspName != NULL )
			free( pBaseTable->pszNspName );
	}

	if ( pTableAr->pData != NULL )
		free( pTableAr->pData );
	
	pTableAr->count = 0;
	pTableAr->pData = NULL;
}

/*-------------------------------------------------------------------------
 *
 * FreeCDBInstanceArray( CDBInstanceArray* pInstanceAr ) returns void
 *
 * This frees the pointers allocated in each of the CBBInstance structs
 * that are part of the pInstanceAr->pData array, and then frees the pData 
 * pointer itself.  It does NOT free the pInstanceAr.
 *-------------------------------------------------------------------------
 */
/*
void FreeCDBInstanceArray( CDBInstanceArray* pInstanceAr )
{
	if ( pInstanceAr == NULL )
		return;
	
	int i;
	for ( i=0; i<pInstanceAr->count; i++ )
	{
		CDBInstance* pInstance = &pInstanceAr->pData[i]; 
			
		if ( pInstance->pszHost != NULL )
			free( pInstance->pszHost );
		if ( pInstance->pszDataDir != NULL )
			free( pInstance->pszDataDir );
	}

	if ( pInstanceAr->pData != NULL )
	{
		free( pInstanceAr->pData );
		pInstanceAr->pData = NULL;
	}
	
	pInstanceAr->count = 0;
}
*/

/*-------------------------------------------------------------------------
 *
 * FreeCDBPolicyArray( CDBPolicyArray* pPolicyAr ) returns void
 *
 * This frees the pointers allocated in each of the CDBPolicy structs
 * that are part of the pPolicyAr->pData array, and then frees the pData 
 * pointer itself.  It does NOT free the pPolicyAr.
 *-------------------------------------------------------------------------
 */
/*
void FreeCDBPolicyArray( CDBPolicyArray* pPolicyAr )
{
	if ( pPolicyAr == NULL )
		return;
	
	int i;
	for ( i=0; i<pPolicyAr->count; i++ )
	{
		CDBPolicy* pPolicy = &pPolicyAr->pData[i]; 
			
		if ( pPolicy->pszTableName != NULL )
			free( pPolicy->pszTableName );
		if ( pPolicy->pszNspName != NULL )
			free( pPolicy->pszNspName );
		if ( pPolicy->pszParameters != NULL )
			free( pPolicy->pszParameters );
	}
	
	if ( pPolicyAr->pData != NULL )
	{
		free( pPolicyAr->pData );
		pPolicyAr->pData = NULL;
	}
	
	pPolicyAr->count = 0;
}
*/

/*-------------------------------------------------------------------------
 *
 * FreeCDBSegArray( CDBSegArray* pSegAr ) returns void
 *
 * This frees the pointers allocated in each of the CDVSeg structs
 * that are part of the pSegAr->pData array, and then frees the pData 
 * pointer itself.  It does NOT free the pSegAr.
 *-------------------------------------------------------------------------
 */
/*
void FreeCDBSegArray( CDBSegArray* pSegAr )
{
	if ( pSegAr == NULL )
		return;
	
	int i;
	for ( i=0; i<pSegAr->count; i++ )
	{
		CDBSeg* pSeg = &pSegAr->pData[i]; 
			
		if ( pSeg->pszDBName != NULL )
			free( pSeg->pszDBName );
		if ( pSeg->pszDBUser != NULL )
			free( pSeg->pszDBUser );
		if ( pSeg->pszDBPswd != NULL )
			free( pSeg->pszDBPswd );
	}

	if ( pSegAr->pData != NULL )
	{
		free( pSegAr->pData );
		pSegAr->pData = NULL;
	}
	
	pSegAr->count = 0;
}
*/

/*-------------------------------------------------------------------------
 *
 * FreeCDBSegmentInstanceArray( CDBSegmentInstanceArray* pSegmentInstanceAr ) returns void
 *
 * This frees the pointers allocated in each of the CDBSegmentInstance structs
 * that are part of the pSegmentInstanceAr->pData array, and then frees the pData 
 * pointer itself.  It does NOT free the pSegmentInstanceAr.
 *-------------------------------------------------------------------------
 */
void FreeCDBSegmentInstanceArray( CDBSegmentInstanceArray* pSegmentInstanceAr )
{
	if ( pSegmentInstanceAr == NULL )
		return;
	
	if ( pSegmentInstanceAr->pData != NULL )
	{
		free( pSegmentInstanceAr->pData );
		pSegmentInstanceAr->pData = NULL;
	}
	
	pSegmentInstanceAr->count = 0;
}

/*-------------------------------------------------------------------------
 *
 * GetCDBAssignmentArray(PGconn* pConn, const char* pszDBName, CDBAssignmentArray* pAssignmentAr ) 
 * returns bool
 *
 * This reads all rows from cdb_assignment, 
 * joining with pg_class and pg_namespace to get the table and schema names.
 *
 * After using it, call FreeCDBAssignmentArray.
 *-------------------------------------------------------------------------
 */
/*
bool GetCDBAssignmentArray(PGconn* pConn, const char* pszDBName, CDBAssignmentArray* pAssignmentAr )
{
	bool		bRtn = false;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;
	
	pAssignmentAr->count = 0;
	pAssignmentAr->pData = NULL;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry, 	" SELECT" 
							"	a.tbloid," 
							"	c.relname,"  
							"	n.nspname,"
							"	a.partno,"
							"	a.segid"
							"	FROM"
							"	cdb_assignment a,"
							"	pg_class c,"
							"	pg_namespace n"
							"	WHERE"
							"	a.tbloid = c.oid"
							" 	AND c.relnamespace = n.oid"
							"	ORDER BY"
							"	2" );

	pRes = PQexec(pConn, pQry->data);
	if ( pRes == NULL || PQresultStatus(pRes) != PGRES_TUPLES_OK )
	{
		write_err_msg("query to obtain list of cdb assignments failed : %s",
				  PQerrorMessage(pConn));
		goto cleanup;
	}

	int ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		write_err_msg("no CDB assignments for database \"%s\"\n", pszDBName);
		goto cleanup;
	}
	
	pAssignmentAr->count = ntups;
	pAssignmentAr->pData = (CDBAssignment *)calloc( ntups, sizeof(CDBAssignment) );
	if ( pAssignmentAr->pData == NULL )
	{
		write_err_msg("cannot allocate memory for query results in GetCDBAssignmentArray\n");
		goto cleanup;
	}

	int i_tbloid	= PQfnumber(pRes, "tbloid");
	int i_relname	= PQfnumber(pRes, "relname");
	int i_nspname	= PQfnumber(pRes, "nspname");
	int i_partno	= PQfnumber(pRes, "partno");
	int i_segid		= PQfnumber(pRes, "segid");

	int i;
	for ( i=0; i<ntups; i++ )
	{
		CDBAssignment* pAssignment = &pAssignmentAr->pData[i]; 
		pAssignment->tbloid			= atoi(PQgetvalue(pRes, i, i_tbloid));
		pAssignment->pszTableName	= strdup(PQgetvalue(pRes, i, i_relname));
		pAssignment->pszNspName		= strdup(PQgetvalue(pRes, i, i_nspname));
		pAssignment->partno			= atoi(PQgetvalue(pRes, i, i_partno));
		pAssignment->segid			= atoi(PQgetvalue(pRes, i, i_segid));
	}
	
	bRtn = true;
	
cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);

	return bRtn;
}
*/

/*-------------------------------------------------------------------------
 *
 * GetCDBBaseTableArray(PGconn* pConn, const char* pszDBName, CDBBaseTableArray* pTableAr ) 
 * returns bool
 *
 * This reads all rows from cdb_basetable, with an optional filter on partitioned.
 *
 * After using it, call FreeCDBBaseTableArray.
 *-------------------------------------------------------------------------
 */

bool GetCDBBaseTableArray(PGconn* pConn, const char* pszDBName, CDBBaseTableArray* pTableAr, bool* pbPartitioned )
{
	bool		bRtn = false;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;
	char*		pszPartitionedWhere = NULL;
	
	pTableAr->count = 0;
	pTableAr->pData = NULL;
	
	if ( pbPartitioned != NULL )
	{
		pszPartitionedWhere = MakeString(" AND b.partitioned = %s", ((*pbPartitioned) ? "true" : "false") );  
	}
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry, 	" SELECT" 
							"	p.localoid," 
							"	c.relname,"  
							"	n.nspname"
							//"	b.partitioned"
							"	FROM"
							"	cdb.policy p,"
							"	pg_class c,"
							"	pg_namespace n"
							"	WHERE"
							"	p.localoid = c.oid"
							" 	AND c.relnamespace = n.oid"
							"%s"
							"	ORDER BY"
							"	2",
							StringNotNull( pszPartitionedWhere, "")
							);

	pRes = PQexec(pConn, pQry->data);

	if ( pRes == NULL || PQresultStatus(pRes) != PGRES_TUPLES_OK )
	{
		write_err_msg("query to obtain list of cdb managed tables failed : %s",
				  PQerrorMessage(pConn));
		goto cleanup;
	}

	int ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		write_err_msg("no CDB managed tables for database \"%s\"\n", pszDBName);
		goto cleanup;
	}
	
	pTableAr->count = ntups;
	pTableAr->pData = (CDBBaseTable *)calloc( ntups, sizeof(CDBBaseTable) );
	if ( pTableAr->pData == NULL )
	{
		write_err_msg("cannot allocate memory for query results in GetCDBBaseTableArray\n");
		goto cleanup;
	}

	int i_tbloid = PQfnumber(pRes, "localoid");
	int i_relname = PQfnumber(pRes, "relname");
	int i_nspname = PQfnumber(pRes, "nspname");
	//int i_partitioned = PQfnumber(pRes, "partitioned");

	int i;
	for ( i=0; i<ntups; i++ )
	{
		CDBBaseTable* pBaseTable = &pTableAr->pData[i]; 
		pBaseTable->tbloid			= atoi(PQgetvalue(pRes, i, i_tbloid));
		pBaseTable->pszTableName	= strdup(PQgetvalue(pRes, i, i_relname));
		pBaseTable->pszNspName		= strdup(PQgetvalue(pRes, i, i_nspname));
		//pBaseTable->partitioned		= (strcmp( PQgetvalue(pRes, i, i_partitioned), "t") == 0);
	}
	
	bRtn = true;
	
cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);

	return bRtn;
}

/*-------------------------------------------------------------------------
 *
 * GetCDBInstanceArray(PGconn* pConn, const char* pszDBName, CDBInstanceArray* pInstanceAr ) 
 * returns bool
 *
 * This reads all rows from cdb_instance.
 *
 * After using it, call FreeCDBInstanceArray.
 *-------------------------------------------------------------------------
 */
/*
bool GetCDBInstanceArray(PGconn* pConn, const char* pszDBName, CDBInstanceArray* pInstanceAr )
{
	bool		bRtn = false;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;
	
	pInstanceAr->count = 0;
	pInstanceAr->pData = NULL;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry, 	" SELECT" 
							"   d.dbid," 
							//"	d.valid,"  
							"	l.port,"
							"	h.hostname,"
							"	l.datadir"
							"	FROM"
							"	cdb.pgdatabase d,"
                              "   cdb.listener l,"
                              "   cdb.host h"
                              "   where d.listenerid = l.listenerid"
                              "   and l.hostid = h.hostid"
							"	ORDER BY"
							"	1" );

	pRes = PQexec(pConn, pQry->data);

	if ( pRes == NULL || PQresultStatus(pRes) != PGRES_TUPLES_OK )
	{
		write_err_msg("query to obtain list of cdb instances failed : %s",
				  PQerrorMessage(pConn));
		goto cleanup;
	}

	int ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		write_err_msg("no CDB instances for database \"%s\"\n", pszDBName);
		goto cleanup;
	}
	
	pInstanceAr->count = ntups;
	pInstanceAr->pData = (CDBInstance *)malloc( ntups * sizeof(CDBInstance) );
	if ( pInstanceAr->pData == NULL )
	{
		write_err_msg("cannot allocate memory for query results in GetCDBInstanceArray\n");
		goto cleanup;
	}

	int i_instid	= PQfnumber(pRes, "dbid");
	//int i_valid		= PQfnumber(pRes, "valid");
	int i_port		= PQfnumber(pRes, "port");
	int i_host		= PQfnumber(pRes, "hostname");
	int i_datadir	= PQfnumber(pRes, "datadir");

	int i;
	for ( i=0; i<ntups; i++ )
	{
		CDBInstance* pInstance = &pInstanceAr->pData[i]; 
		pInstance->instid			= atoi(PQgetvalue(pRes, i, i_instid));
		//pInstance->valid			= (strcmp( PQgetvalue(pRes, i, i_valid), "t") == 0);
		pInstance->port				= atoi(PQgetvalue(pRes, i, i_port));
		if ( PQgetisnull(pRes, i, i_host))
			pInstance->pszHost = NULL;
		else
			pInstance->pszHost			= strdup(PQgetvalue(pRes, i, i_host));
		
		if ( PQgetisnull(pRes, i, i_datadir))
			pInstance->pszDataDir = NULL;
		else
			pInstance->pszDataDir = strdup(PQgetvalue(pRes, i, i_datadir));
	}
	
	bRtn = true;
	
cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);

	return bRtn;
}
*/

/*-------------------------------------------------------------------------
 *
 * GetCDBPolicyArray(PGconn* pConn, const char* pszDBName, CDBPolicyArray* pPolicyAr ) 
 * returns bool
 *
 * This reads all rows from cdb_policy, 
 * joining with pg_class and pg_namespace to get the table and schema names.
 *
 * After using it, call FreeCDBPolicyArray.
 *-------------------------------------------------------------------------
 */
 /*
bool GetCDBPolicyArray(PGconn* pConn, const char* pszDBName, CDBPolicyArray* pPolicyAr )
{
	bool		bRtn = false;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;

	pPolicyAr->count = 0;
	pPolicyAr->pData = NULL;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry, 	" SELECT" 
							"	p.localoid," 
							"	c.relname,"  
							"	n.nspname,"
							//"	p.seqno,"
							"	p.attrnums"
							//"	p.nparts,"
							//"	p.type,"
							//"	p.parameters"
							"	FROM"
							"	cdb.policy p,"
							"	pg_class c,"
							"	pg_namespace n"
							"	WHERE"
							"	p.localoid = c.oid"
							" 	AND c.relnamespace = n.oid"
							"	ORDER BY"
							"	2" );

	pRes = PQexec(pConn, pQry->data);

	if ( pRes == NULL || PQresultStatus(pRes) != PGRES_TUPLES_OK )
	{
		write_err_msg("query to obtain list of cdb policies failed : %s",
				  PQerrorMessage(pConn));
		goto cleanup;
	}

	int ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		write_err_msg("no CDB policies for database \"%s\"\n", pszDBName);
		goto cleanup;
	}
	
	pPolicyAr->count = ntups;
	pPolicyAr->pData = (CDBPolicy *)malloc( ntups * sizeof(CDBPolicy) );
	if ( pPolicyAr->pData == NULL )
	{
		write_err_msg("cannot allocate memory for query results in GetCDBPolicyArray\n");
		goto cleanup;
	}

	int i_tbloid		= PQfnumber(pRes, "localoid");
	int i_relname		= PQfnumber(pRes, "relname");
	int i_nspname		= PQfnumber(pRes, "nspname");
	//int i_seqno			= PQfnumber(pRes, "seqno");
	//int i_attnum		= PQfnumber(pRes, "attrnums");
	//int i_nparts		= PQfnumber(pRes, "nparts");
	//int i_type			= PQfnumber(pRes, "type");
	//int i_parameters	= PQfnumber(pRes, "parameters");

	int i;
	for ( i=0; i<ntups; i++ )
	{
		CDBPolicy* pPolicy = &pPolicyAr->pData[i]; 
		pPolicy->tbloid			= atoi(PQgetvalue(pRes, i, i_tbloid));
		pPolicy->pszTableName	= strdup(PQgetvalue(pRes, i, i_relname));
		pPolicy->pszNspName		= strdup(PQgetvalue(pRes, i, i_nspname));
		//pPolicy->seqno			= atoi(PQgetvalue(pRes, i, i_seqno));
		//pPolicy->attnum			= atoi(PQgetvalue(pRes, i, i_attnum));
		//pPolicy->nparts			= atoi(PQgetvalue(pRes, i, i_nparts));
		
        /*
		if ( PQgetisnull(pRes, i, i_type))
			pPolicy->type	= '\0';
		else
			pPolicy->type			= (PQgetvalue(pRes, i, i_type))[0];
		
		if ( PQgetisnull(pRes, i, i_parameters))
			pPolicy->pszParameters = NULL;
		else
			pPolicy->pszParameters	= strdup(PQgetvalue(pRes, i, i_parameters));
	    * /
    }
	
	bRtn = true;
	
cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);

	return bRtn;
}
*/

/*-------------------------------------------------------------------------
 *
 * GetCDBSegArray(PGconn* pConn, const char* pszDBName, CDBSegArray* pSegAr ) 
 * returns bool
 *
 * This reads all rows from cdb_seg.
 *
 * After using it, call FreeCDBSegArray.
 *-------------------------------------------------------------------------
 */
 /*
bool GetCDBSegArray(PGconn* pConn, const char* pszDBName, CDBSegArray* pSegAr )
{
	bool		bRtn = false;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;

	pSegAr->count = 0;
	pSegAr->pData = NULL;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry, 	" SELECT" 
							"	d.dbid," 
							//"	primary_inst,"  
							//"	valid,"
							"	d.dbname,"
							"	l.cdbsuperuser,"
							"	l.cdbsuperpswd"
							"	FROM"
                              "   cdb.pgdatabase d,"
							"	cdb.listener l"
                              "   where d.listenerid = l.listenerid"
							"	ORDER BY"
							"	1" );

	pRes = PQexec(pConn, pQry->data);
	if ( pRes == NULL || PQresultStatus(pRes) != PGRES_TUPLES_OK )
	{
		write_err_msg("query to obtain list of cdb segs failed : %s",
				  PQerrorMessage(pConn));
		goto cleanup;
	}

	int ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		write_err_msg("no CDB segs for database \"%s\"\n", pszDBName);
		goto cleanup;
	}
	
	pSegAr->count = ntups;
	pSegAr->pData = (CDBSeg *)malloc( ntups * sizeof(CDBSeg) );
	if ( pSegAr->pData == NULL )
	{
		write_err_msg("cannot allocate memory for query results in GetCDBSegArray\n");
		goto cleanup;
	}

	int i_segid			= PQfnumber(pRes, "dbid");
	//int i_primary_inst	= PQfnumber(pRes, "primary_inst");
	//int i_valid			= PQfnumber(pRes, "valid");
	int i_dbname		= PQfnumber(pRes, "dbname");
	int i_dbuser		= PQfnumber(pRes, "cdbsuperuser");
	int i_dbpswd		= PQfnumber(pRes, "cdbsuperpswd");

	int i;
	for ( i=0; i<ntups; i++ )
	{
		CDBSeg* pSeg = &pSegAr->pData[i]; 
		pSeg->segid			= atoi(PQgetvalue(pRes, i, i_segid));
		//pSeg->primary_inst	= atoi(PQgetvalue(pRes, i, i_primary_inst));
		//pSeg->valid			= (strcmp( PQgetvalue(pRes, i, i_valid), "t") == 0);
		if ( PQgetisnull(pRes, i, i_dbname))
			pSeg->pszDBName = NULL;
		else
			pSeg->pszDBName	= strdup(PQgetvalue(pRes, i, i_dbname));
		
		if ( PQgetisnull(pRes, i, i_dbuser))
			pSeg->pszDBUser = NULL;
		else
			pSeg->pszDBUser	= strdup(PQgetvalue(pRes, i, i_dbuser));
		
		if ( PQgetisnull(pRes, i, i_dbpswd))
			pSeg->pszDBPswd = NULL;
		else
			pSeg->pszDBPswd	= strdup(PQgetvalue(pRes, i, i_dbpswd));
	}
	
	bRtn = true;
	
cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);

	return bRtn;
}
*/

/*-------------------------------------------------------------------------
 *
 * GetCDBSegmentInstanceArray(PGconn* pConn, const char* pszDBName, CDBSegmentInstanceArray* pSegmentInstanceAr ) 
 * returns bool
 *
 * This reads all rows from cdb_segment_instance.
 *
 * After using it, call FreeCDBSegmentInstanceArray.
 *-------------------------------------------------------------------------
 */
bool GetCDBSegmentInstanceArray(PGconn* pConn, const char* pszDBName, CDBSegmentInstanceArray* pSegmentInstanceAr )
{
	bool		bRtn = false;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;

	pSegmentInstanceAr->count = 0;
	pSegmentInstanceAr->pData = NULL;
	
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry, 	" SELECT" 
							"	d.dbid,"
//                              "   d.isprimary,"
                              "   d.dbname,"
                              "   h.hostname,"
                              "   l.port," 
                              "   l.datadir,"
                              "   l.cdbsuperuser"
							"	FROM"
							"	cdb.pgdatabase d,"
                              "   cdb.listener l,"
                              "   cdb.host h"
                              "   where d.listenerid = l.listenerid"
                              "   and l.hostId = h.hostid" );

	pRes = PQexec(pConn, pQry->data);
	if ( pRes == NULL || PQresultStatus(pRes) != PGRES_TUPLES_OK )
	{
		write_err_msg("query to obtain list of cdb segment instances failed : %s",
				  PQerrorMessage(pConn));
		goto cleanup;
	}

	int ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		write_err_msg("no CDB segment instances for database \"%s\"\n", pszDBName);
		goto cleanup;
	}
	
	pSegmentInstanceAr->count = ntups;
	pSegmentInstanceAr->pData = (CDBSegmentInstance *)malloc( ntups * sizeof(CDBSegmentInstance) );
	if ( pSegmentInstanceAr->pData == NULL )
	{
		write_err_msg("cannot allocate memory for query results in GetCDBSegmentInstanceArray\n");
		goto cleanup;
	}

	int i_segid		= PQfnumber(pRes, "dbid");
    //bool i_isPrimary = PQfnumber(pRes, "isPrimary");
    int i_dbname    = PQfnumber(pRes, "dbname");
    int i_hostname  = PQfnumber(pRes, "hostname");
    int i_port      = PQfnumber(pRes, "port");
    int i_datadir   = PQfnumber(pRes, "datadir");
	int i_superuser = PQfnumber(pRes, "cdbsuperuser");

	int i;
	for ( i=0; i<ntups; i++ )
	{
		CDBSegmentInstance* pSegmentInstance = &pSegmentInstanceAr->pData[i]; 
        pSegmentInstance->segid		= atoi(PQgetvalue(pRes, i, i_segid));
        pSegmentInstance->dbname      = PQgetvalue(pRes, i, i_dbname);
        pSegmentInstance->dbhostname  = PQgetvalue(pRes, i, i_hostname);
        pSegmentInstance->dbport      = atoi(PQgetvalue(pRes, i, i_port));
        pSegmentInstance->dbdatadir   = PQgetvalue(pRes, i, i_datadir);
        pSegmentInstance->dbsuperuser = PQgetvalue(pRes, i, i_superuser);
	}
	
	bRtn = true;
	
cleanup:
	if ( pRes != NULL )
		PQclear(pRes);
	if ( pQry != NULL )
		destroyPQExpBuffer(pQry);

	return bRtn;
}

