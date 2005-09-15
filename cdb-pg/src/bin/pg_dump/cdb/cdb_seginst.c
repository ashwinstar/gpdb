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

/* static functions used in set manipulation */
static int cmpInstSeg( const void* l, const void* r );

/*
 * CountInstanceHosts : returns the number of elts of InstSeg array that have their
 * instVal defined by a host string.
 */
int CountInstanceHosts( CDBSet* pSet )
{
	int i;
	int x;

	if ( pSet->type != SET_INDIVIDUAL || pSet->count == 0 )
		return 0;
	
	x = 0;
	for ( i=0; i<pSet->count; i++ )
	{
		if ( pSet->pData[i].instVal.val_type == DEFN_TYPE_HOST )
			x++;
	}
	
	return x;
}

/* FillCDBSet: This function reads a control file, and parses each line 
 * into an instid  and segid using a regular expression.
 * The valid formats are:
 * a   b, where a and b are either integers (>= 0) or *, meaning match all.
 * or host:port b where b is an integer( >= 0) and host is either an ip address
 * or a hostname, and port is an integer.
 * The values are stored in an array which is then sorted 
 * for later fast binary searches. 
 */
bool FillCDBSet( CDBSet* pSet, char* pszControlFile )
{
	/* Start out with space for 20 elements. */
	bool		bRtn	= true;
	FILE*		f		= NULL;
	char*		fbuf	= NULL;

	char* pBeg;
	char* pEnd;
	
	static regex_t rFinder;
	static regex_t rWhitespace;
	static bool bCompiled;
	
	struct stat buf;
	int lineno;
	int rtn;
	regmatch_t matches[11];
	char errbuf[1024];
	InstSeg* newp;
	int index;

	int nAllocated = 20;
	pSet->type = SET_INDIVIDUAL;
	pSet->count = 0;
	pSet->pData = (InstSeg *)calloc( nAllocated, sizeof(InstSeg) );
	if ( pSet->pData == NULL )
	{
		write_err_msg( "Unable to allocate memory for MPP set information\n" );
		bRtn = false;
		goto cleanup;
	}
	
	/* Make sure that the file exists, and get its size in bytes. */
	if ( 0 > stat( pszControlFile, &buf ) )
	{
		write_err_msg( "Error accessing backup control file %s\n", pszControlFile );
		bRtn = false;
		goto cleanup;
	}
	
	if ( buf.st_size > INT_MAX )
	{
		write_err_msg( "Backup control file %s too large.\n", pszControlFile );
		bRtn = false;
		goto cleanup;
	}
	
	/* Allocate enough memory for entire file, and read it in. */
	fbuf = (char *)malloc( buf.st_size + 1 );
	if ( fbuf == NULL )
	{
		write_err_msg( "Unable to allocate memory for control file contents\n" );	
		bRtn = false;
		goto cleanup;
	}
	
	f = fopen( pszControlFile, "r" );
	if ( f == NULL )
	{
		write_err_msg( "Error opening control file %s\n", pszControlFile );	
		bRtn = false;
		goto cleanup;
	}
	
	if ( buf.st_size != fread( fbuf, 1, buf.st_size, f ) )
	{
		write_err_msg( "Error reading control file %s\n", pszControlFile );	
		bRtn = false;
		goto cleanup;
	}
	
	fclose( f );
	f = NULL;
	fbuf[buf.st_size ] = '\0';
	
	/* Allocate enough memory for entire file, and read it in. */
	pBeg = fbuf;
	
	bCompiled = false;
	
	/* Compile the regex on first use. */
	if ( !bCompiled )
	{
		/*
		 *  We are matching lines that have 2 items on them: the first is an instance indicator, the second a segment indicator.
		 * The instance can be either a number (the instid), a * (for all), and ip address, or a host name.
		 * The segment can be either a number (the segid) or a * for all.
		 * 
		 * Here's a breakdown of the complicated regular expression:
		 * It's really ^[[:space:]]*{instance}([[:space:]]+)){segment}[[:space:]]*$
		 * meaning 0 or more spaces, match the instance, 1 or more spaces, match the segment, and zero or more spaces til the end of string.
		 * where {instance} is (([0-9]+)|(\\*)|([0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}:([0-9]{1,5}))|([[:alnum:]]+):([0-9]{1,5}))
		 * which is the or'ing of: [0-9]+ (an instid)
		 * 							\\* (an asterik)
		 *							[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}:[0-9]{1,5}  (an ip with 4 1 to 3 digit numbers separated by ., : a port number
		 *							[[:alnum]]:[0-9]{1-5} (any host name : a port number)
		 *
		 * and where {segment} is ([0-9]+)|(\\*) 
		 * which is the or'ing of: [0-9]+ (an instid)
		 * 							\\* (an asterik)
		 * 
		 * Parentheses are used to form match groups to know which was matched for a particular line. 
		 */
		if ( 0 != regcomp( &rFinder, "^[[:space:]]*"
		"(([0-9]+)|(\\*)|([0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}):([0-9]{1,5})|([[:alnum:]]+):([0-9]{1,5}))"
									"[[:space:]]*"
									"(([0-9]+)|(\\*))"
									"[[:space:]]*$", 
									REG_EXTENDED ) )
		{
			write_err_msg( "Error compiling regular expression for reading control file %s\n", pszControlFile );	
			bRtn = false;
			goto cleanup;
		}
		
		if ( 0 != regcomp( &rWhitespace, "^[[:space:]]*$", REG_EXTENDED ) )
		{
			write_err_msg( "Error compiling regular expression for reading control file %s\n", pszControlFile );	
			bRtn = false;
			goto cleanup;
		}
		
		bCompiled = true;
		assert( rFinder.re_nsub == 10 );
	}
	
	lineno = 0;
	
	/* Loop through the input buffer, finding \n's.  Then operate on each line separately. */
	while ( pBeg != NULL && *pBeg != '\0' )
	{
		pEnd = strchr( pBeg, '\n' );
		if ( pEnd != NULL )
			*pEnd = '\0';
		
		lineno++;
		
		// Is line entirely whitespace?  If so, continue
		rtn = regexec( &rWhitespace, pBeg, 0, NULL, 0 );
		if ( rtn == 0 )
			continue;
		
		/* match the line against the regex.  If it fails, print lineno and error message */
		rtn = regexec( &rFinder, pBeg, 11, matches, 0 );
		if ( rtn != 0 )
		{
			regerror( rtn, &rFinder, errbuf, 1024 );
			write_err_msg( "Error on line %d of file of control file %s: not a valid line: %s\n", lineno, pszControlFile, errbuf );
			bRtn = false;
			goto cleanup;
		}
		
		index = pSet->count;
		
		/* reallocate array if necessary. */
		if ( index == nAllocated )
		{
			// Reallocate the memory
			newp = (InstSeg *)realloc( pSet->pData, 2 * nAllocated * sizeof(InstSeg) );
			if ( newp == NULL )
			{
				write_err_msg( "Error reallocating memory for InstSeg array: couldn't allocate %d bytes\n", nAllocated * sizeof(InstSeg) );
				bRtn = false;
				goto cleanup;
			}
			
			pSet->pData = newp;
			memset( pSet->pData + nAllocated, 0, nAllocated * sizeof(InstSeg) );
			nAllocated *= 2;
		}
		
		pSet->pData[index].instVal.val_type = DEFN_TYPE_ID;
		pSet->pData[index].instVal.val_id	= -1;
		pSet->pData[index].instVal.val_pszHost	= NULL;
		pSet->pData[index].instVal.val_port	= -1;
		
		pSet->pData[index].segVal.val_type	= DEFN_TYPE_ID;
		pSet->pData[index].segVal.val_id	= -1;
		
		/* Grab instance and segment ids from regex_t object */
		assert( matches[2].rm_so >= 0 ||
				matches[3].rm_so >= 0 || 
				( matches[4].rm_so >= 0 && matches[5].rm_so >= 0 ) ||   
				( matches[6].rm_so >= 0 && matches[7].rm_so >= 0 ) );   
				
		if ( matches[2].rm_so >= 0 )
		{
			pSet->pData[index].instVal.val_type = DEFN_TYPE_ID;
			pSet->pData[index].instVal.val_id = GetMatchInt( &matches[2], pBeg );
		}
		else if ( matches[3].rm_so >= 0 )
		{
			pSet->pData[index].instVal.val_type = DEFN_TYPE_ALL;
		}
		else if ( matches[4].rm_so >= 0 )
		{
			pSet->pData[index].instVal.val_type = DEFN_TYPE_HOST;
			pSet->pData[index].instVal.val_pszHost = GetMatchString( &matches[4], pBeg );
			pSet->pData[index].instVal.val_port = GetMatchInt( &matches[5], pBeg );
		}
		else if ( matches[6].rm_so >= 0 )
		{
			pSet->pData[index].instVal.val_type = DEFN_TYPE_HOST;
			pSet->pData[index].instVal.val_pszHost = GetMatchString( &matches[6], pBeg );
			pSet->pData[index].instVal.val_port = GetMatchInt( &matches[7], pBeg );
		}
		
		assert( matches[9].rm_so >= 0 ||
				matches[10].rm_so >= 0 ); 

		if ( matches[9].rm_so >= 0 )
		{
			pSet->pData[index].segVal.val_type = DEFN_TYPE_ID;
			pSet->pData[index].segVal.val_id = GetMatchInt( &matches[9], pBeg );
		}
		else if ( matches[10].rm_so >= 0 )
		{
			pSet->pData[index].segVal.val_type = DEFN_TYPE_ALL;
		}
		
		// We disallow the case of an instance host and a * segid, since we would have no way of
		// knowing what they would be.
		if ( pSet->pData[index].segVal.val_type == DEFN_TYPE_ALL &&
			pSet->pData[index].instVal.val_type == DEFN_TYPE_HOST )
		{
			write_err_msg( "Error on line %d of control file %s: Cannot specify an instance by host ip or a segment as all.\n",
							lineno, pszControlFile);
			bRtn = false;
			goto cleanup;
		}
		
		if ( pEnd != NULL )
		{
			*pEnd = '\n';
			pBeg = pEnd + 1;
		}
		else
			pBeg = NULL;
		
		pSet->count++;
	}
	
	/* sort the results for easy lookup later */
	qsort( pSet->pData, pSet->count, sizeof(InstSeg), cmpInstSeg );

cleanup:	
	if ( fbuf != NULL )
		free( fbuf );
	if ( f != NULL )
		fclose( f );
	
	return bRtn;
}

/* FindInCDBSet: This function determines whether a instid, segid pair meets the definition
 * in a Set object. It looks at the type value.  THis will determine the answer except when
 * the type is SET_INDIVIDUAL.  In that case, we have to do bsearches into the pData array.
 */
bool FindInCDBSet( CDBSet* pSet, int instID, int segID )
{
	InstSeg test;
	switch ( pSet->type )
	{
		case SET_ALL:
			return true;
		case SET_TAILS_ONLY:
			return (instID == 0);
		case SET_HEAD_ONLY:
			return (instID == 1);
		default:
			break;
	}	
	
	/* See whether *, * is in pData */
	test.instVal.val_type = DEFN_TYPE_ALL;
	test.segVal.val_type = DEFN_TYPE_ALL;
	
	if ( NULL != bsearch( &test, pSet->pData, pSet->count, sizeof(InstSeg), cmpInstSeg ) )
		return true;
	
	/* See whether instID, * is in pData */
	test.instVal.val_type	= DEFN_TYPE_ID;
	test.instVal.val_id		= instID;
	test.segVal.val_type 	= DEFN_TYPE_ALL;
	if ( NULL != bsearch( &test, pSet->pData, pSet->count, sizeof(InstSeg), cmpInstSeg ) )
		return true;
	
	/* See whether *, segID is in pData */
	test.instVal.val_type	= DEFN_TYPE_ALL;
	test.segVal.val_type	= DEFN_TYPE_ID;
	test.segVal.val_id	= segID;
	if ( NULL != bsearch( &test, pSet->pData, pSet->count, sizeof(InstSeg), cmpInstSeg ) )
		return true;
	
	/* See whether instID, segID is in pData */
	test.instVal.val_type	= DEFN_TYPE_ID;
	test.instVal.val_id		= instID;
	test.segVal.val_type	= DEFN_TYPE_ID;
	test.segVal.val_id		= segID;
	if ( NULL != bsearch( &test, pSet->pData, pSet->count, sizeof(InstSeg), cmpInstSeg ) )
		return true;
	
	return false;
}

/* FreeCDBSet: This function frees the memory associated with the fields of the CDBSet object
 * It does not free the passed pointer, however.
 */
void FreeCDBSet( CDBSet* pSet )
{
	int i;
	if ( pSet == NULL || pSet->count == 0 || pSet->pData == NULL )
		return;
	
	for ( i=0; i<pSet->count; i++ )
	{
		InstSeg* pInstSeg = &pSet->pData[i];
		if ( pInstSeg->instVal.val_pszHost != NULL )
			free( pInstSeg->instVal.val_pszHost );
	}
	
	free( pSet->pData );
}

/* FreeSegmentInstanceArray: This function frees the memory allocated for ther pSegInstAr->pData array,
 * and the strings that were allocated for each element in the array
 * It does NOT free the pSegInstAr itself.
 */
void FreeSegmentInstanceArray( SegmentInstanceArray* pSegInstAr )
{
	int i;
	if ( pSegInstAr == NULL )
		return;
	
	for ( i=0; i<pSegInstAr->count; i++ )
	{
		SegmentInstance* pSegInst = &pSegInstAr->pData[i];
			
		if ( pSegInst->pszHost != NULL )
			free( pSegInst->pszHost );
        /*
		if ( pSegInst->pszDBName != NULL )
			free( pSegInst->pszDBName );
         */
		if ( pSegInst->pszDBUser != NULL )
			free( pSegInst->pszDBUser );
		if ( pSegInst->pszDBPswd != NULL )
			free( pSegInst->pszDBPswd );
	}

	if ( pSegInstAr->pData != NULL )
		free( pSegInstAr->pData );
	
	pSegInstAr->count = 0;
	pSegInstAr->pData = NULL;
}

/* GetSegmentInstanceArray: This function reads all active instance segment pairs from the function mpp_segment_instance_map().
 * It then uses the set specification fo filter out any instid, segid combinations
 * that don't match the set.  bExcludeHead = true is used for restoring, because
 * the head is restored first seperately from the rest of the databases. 
 /
*/

bool GetSegmentInstanceArray(PGconn* pConn, char* pszDBName, CDBSet* pSet, SegmentInstanceArray* pSegInstAr, bool bExcludeHead )
{
	bool		bRtn = true;
	PQExpBuffer pQry = NULL;
	PGresult*	pRes = NULL;
	int ntups;
	int count;
	int i_segid;
	int i_instid;
    int i_isvalid;
    int i_isprimary;
	int i_host;
	int i_port;
	int i_dbuser;
	int i_dbpswd;
	int i;
	int x;
	SegmentInstance* pSegInst;
	
	/* Now select from mpp_segment_instance_map() */
	pQry = createPQExpBuffer();
	appendPQExpBuffer(pQry,  "SELECT" 
							 " d.dbid," 
							 " d.content,"  
							 " d.valid,"
                               " d.isPrimary,"
							 " h.hostname,"
							 " l.port,"
							 " l.mppsuperuser,"
							 " l.mppsuperpswd"
							 " FROM"
							 " mpp.pgdatabase d,"
                               " mpp.listener l,"
                               " mpp.host h"
							 " WHERE d.listenerid = l.listenerid"
                               " and l.hostid = h.hostid and d.valid = 't'"
							 );

	pRes = PQexec(pConn, pQry->data);
	if (!pRes || PQresultStatus(pRes) != PGRES_TUPLES_OK)
	{
		write_err_msg("query to obtain list of MPP segments and instances failed: %s",
				  PQerrorMessage(pConn));
		bRtn = false;
		goto cleanup;		  
	}
	
	ntups = PQntuples(pRes);
	if (ntups <= 0)
	{
		write_err_msg("no MPP segment/instances for database \"%s\"\n", pszDBName);
		bRtn = false;
		goto cleanup;		  
	}
	
	count = ntups + 1;
	// See how many set elements there are that were specified by HostIP or name.
	count += CountInstanceHosts( pSet );

	/* Allocate enough memory for all of them, even though some may be filtered out.  */
	pSegInstAr->count = 0;
	pSegInstAr->pData = (SegmentInstance *) calloc( count,  sizeof(SegmentInstance) );
	if ( pSegInstAr->pData == NULL )
	{
		write_err_msg("Unable to allocate memory for MPP segment/instances information\n");
		bRtn = false;
		goto cleanup;		  
	}

	i_segid		= PQfnumber(pRes, "dbid");
	i_instid	= PQfnumber(pRes, "content");
	i_isvalid	= PQfnumber(pRes, "valid");
	i_host		= PQfnumber(pRes, "hostname");
	i_port		= PQfnumber(pRes, "port");
	i_dbuser	= PQfnumber(pRes, "mppsuperuser");
	i_dbpswd	= PQfnumber(pRes, "mppsuperpswd");
    i_isprimary = PQfnumber(pRes, "isPrimary");

	/* Read through the results set.  */
	x = 0;
	for ( i=0; i<ntups; i++ )
	{
		int segid	= atoi(PQgetvalue(pRes, i, i_segid));
		int instid	= (atoi(PQgetvalue(pRes, i, i_instid))==-1)?1:0;
         int isprimary = (strcmp(PQgetvalue(pRes, i, i_isprimary), "t")==0);
         
         /* skip mirror qd node */
         if (instid && !isprimary)
            continue;
			
		/* See whether this instid, segid pair is part of the set definition.  */
		if ( !FindInCDBSet( pSet, instid, segid ) )
			continue;
						
		pSegInst = &pSegInstAr->pData[x++];
		pSegInst->segid		= segid; 
		pSegInst->instid   	= instid;
		pSegInst->port		= atoi(PQgetvalue(pRes, i, i_port));
		pSegInst->isValid	= ( strcmp(PQgetvalue(pRes, i, i_isvalid), "t") == 0 );
		pSegInst->pszHost	= strdup(PQgetvalue(pRes, i, i_host));
        pSegInst->pszDBName = pszDBName;
		pSegInst->pszDBUser	= strdup(PQgetvalue(pRes, i, i_dbuser));
	
		if ( PQgetisnull(pRes, i, i_dbpswd))
			pSegInst->pszDBPswd = NULL;
		else
			pSegInst->pszDBPswd = strdup(PQgetvalue(pRes, i, i_dbpswd));
	}
	
	/* set the count to be the number that passed the set inclusion test */
	pSegInstAr->count = x;
	
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


