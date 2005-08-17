/*-------------------------------------------------------------------------
 *
 * cdblink.c
 *
 *    Setting up libpq connections to qRxecs from Dispatcher
 *
 *
 * ATTRIBUTION
 *
 *     Concept and initial logic based on contrib/dblink by:
 *
 *     Joe Conway <mail@joeconway.com>
 *     And contributors:
 *     Darko Prenosil <Darko.Prenosil@finteh.hr>
 *     Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 *     Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *     ALL RIGHTS RESERVED;
 *
 *     Permission to use, copy, modify, and distribute this software and its
 *     documentation for any purpose, without fee, and without a written agreement
 *     is hereby granted, provided that the above copyright notice and this
 *     paragraph and the following two paragraphs appear in all copies.
 *
 *     IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 *     DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 *     LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 *     DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 *     THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 *     INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 *     AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 *     ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 *     PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 *
 */
#include "postgres.h"

#include <ctype.h>
#include <pthread.h>

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "nodes/makefuncs.h"

#include "cdb/cdbcat.h"
#include "cdb/cdbconn.h"
#include "cdb/cdblink.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbdisp.h"


/*
 * Points to the anchor block (CdbClusterDescriptor) of the cluster
 * definition and connection blocks.  The block is located in 
 * TopMemoryContext, is allocated at connection time (by cdblink_setup),
 * and persists for the life of the Postgres backend.
 */
CdbClusterDescriptor	*CdbClusterDefinition = NULL;

/*
 * Points to the result of getCdbComponentDatabases()
 */
static CdbComponentDatabases *cdb_component_dbs	= NULL;

/*
 *	buildCdbClusterDefinition reads the Cdb catalog tables and build a CdbComponentDatabases structure.
 *	It then converts this to a CdbClusterDefinition structure and initializes
 *	all the non-connection related fields.
 */
static CdbClusterDescriptor *buildCdbClusterDefinition(void);



/* general utility */
#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))


/* 
 * Called by backend/utils/init/postinit.c to setup stuffe
 *
 * Note -- this function is called with TopTransactionContext *before* 
 *      MessageContext and the other statement-oriented contexts are
 *      allocated.  We want to store the SegmentDbArray and the
 *      heap returned by getCdbSegmentInstances() in TopMemoryContext
 *      (basically permanent storage).  TopTransactionContext is 
 *      reset shortly after this routine is called.
 */
 
void
cdblink_setup( void )
{		
	/* If cdb_role is UTILITY, skip this call. 
	 * If cdb_role is EXECUTE, we need to call buildCdbClusterDefinition()
	 * so we know who we are and what the cluster specification is.
	 * But then we leave, as no connections are required in EXECUTE role.
	 */

	
	if ( Cdb_role == CDB_ROLE_UTILITY )
		return;

	getCdbCdb();
		
	if ( Cdb_role == CDB_ROLE_EXECUTE )
		return;

	
	elog(DEBUG1, "CDBLINK_SETUP---" );	
	
	/*
	 * buildCdbClusterDefinition() calls getCdbComponentDatabases()
	 * to read the Cdb catalog tables and build a CdbComponentDatabases structure.
	 * It then converts this to a CdbClusterDefinition structure and initializes
	 * all the non-connection related fields.
	 */
	CdbClusterDefinition = buildCdbClusterDefinition();
	Assert( CdbClusterDefinition != NULL );
	
	return;
}



/* 
 * Returns the number of segments
 * 
 * N.B.  Cdb_role must be either dispatch or execute, since
 * when utiliy  no cdb catalog tables are read.  An Assert is
 * thrown if Cdb_role = utility.
 */
int getCdbSegmentCount(void)
{
	Assert( Cdb_role != CDB_ROLE_UTILITY );

	return CdbIdentity.numsegments;
}

/* 
 * Returns an array of the segindexes, and fills in the segment count
 * in the out parameter.
 * 
 * N.B.  Cdb_role must be either dispatch or execute, since
 * when utiliy  no cdb catalog tables are read.  An Assert is
 * thrown if Cdb_role = utility.
 */
int *getCdbSegments(int *segcount)
{
	int *segs = NULL;
	int i;

	Assert( segcount != NULL );
	Assert( Cdb_role != CDB_ROLE_UTILITY );
	Assert( CdbClusterDefinition != NULL && 
			CdbClusterDefinition->segdb_count > 0 && 
			CdbClusterDefinition->segment_descriptors != NULL);

	segs = (int *)palloc0( CdbClusterDefinition->segdb_count * sizeof(int) );
	for ( i=0; i<CdbClusterDefinition->segdb_count; i++ )
	{
		segs[i] = CdbClusterDefinition->segment_descriptors[i].segindex;
	}

	*segcount = CdbClusterDefinition->segdb_count;
	return segs;
}

/*
 *	buildCdbClusterDefinition: reads the Cdb catalog tables and build a CdbComponentDatabases structure.
 *	It then converts this to a CdbClusterDefinition structure and initializes
 *	all the non-connection related fields.
 */
CdbClusterDescriptor *buildCdbClusterDefinition(void)
{
	CdbClusterDescriptor 		*ClusterDefn = NULL;
	SegmentDescriptor			*segDesc;
	SegmentDatabaseDescriptor	*segdbDesc;
	CdbComponentDatabaseInfo	*p;
	int i;
	int seg_count = 0; 
	int last_segindex;
	
	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;
	
	/*
	 * Get the structure representing the configured segment databases.  
	 * This structure is fetched into a permanent memory context -- probably the TopMemoryContext.  We
	 * also allocate space for and partially construct the data structure
	 * pointed to by ClusterDefn and headed by CdbClusterDescriptor.
	 */
    oldContext = MemoryContextSwitchTo(permanentContext);

	ClusterDefn = (CdbClusterDescriptor *)palloc0( sizeof(CdbClusterDescriptor) );

	cdb_component_dbs = getCdbComponentDatabases();
	
	ClusterDefn->all_valid_segdbs_connected	= false;
	
	if ( cdb_component_dbs != NULL && cdb_component_dbs->total_segments != 0 )
	{
		ClusterDefn->segment_count	= cdb_component_dbs->total_segments;
		ClusterDefn->segdb_count	= cdb_component_dbs->total_segment_dbs;
		ClusterDefn->segment_descriptors = 
			(SegmentDescriptor *) palloc0 ( ClusterDefn->segment_count * sizeof( SegmentDescriptor ) );
	}
	else
    {
        ClusterDefn->segment_count = 0;
		ClusterDefn->segment_descriptors = NULL;
    }

	MemoryContextSwitchTo(oldContext);

    if ( !ClusterDefn->segment_descriptors )
    {
        elog(NOTICE, "No CDB segment/instance pairs configured");
        return ClusterDefn;
    }

	Assert( cdb_component_dbs->total_segment_dbs > 0 );
		

		
	/*
	 * Pass through each segment database in segment_dbs
	 * and Count up databases per segment_index.  Use this to initialize
	 * the fields in ClusterDefn->segment_descriptors
	 */
	
	
	/*
	 * We loop through the segment_db_info.  Each item has a segindex.
	 * They are sorted by segindex, and there can be > 1 segment_db_info for
	 * a given segindex (currently, there can be 1 or 2)
	 * segDesc points to the SegmentDescriptor for the segindex
	 */
	segDesc = ClusterDefn->segment_descriptors - 1;
	
		
	for ( i=0; i<cdb_component_dbs->total_segment_dbs; i++ )
	{
		p = &cdb_component_dbs->segment_db_info[i];
		if ( i == 0 || (p->segindex != last_segindex) )
		{
			/*
			 * We have a new segindex.  Advance segDesc, and set count to 1.
			 */
			segDesc++;
			Assert( segDesc - ClusterDefn->segment_descriptors < ClusterDefn->segment_count );
			last_segindex = p->segindex;
			segDesc->segindex = p->segindex;
			segDesc->db_count = 1;
			segDesc->error_message = NULL;
			segDesc->query_db = NULL;
			segDesc->sql = NULL;
			seg_count++;
		}
		else
		{
			/*
			 * We have another segdb for the same segindex.  Bump up the count.
			 * It should never go above 2, since we can only have 1 secondary
			 * segdb in the current design.
			 */
			segDesc->db_count++;
			Assert( segDesc->db_count <= 2 );
		}
	}
		
	Assert( seg_count == ClusterDefn->segment_count );
		
	/*
	 * Loop through each segment descriptor in ClusterDefn->segment_descriptors
	 * to allocate and initialize the db_descriptors array.
	 */
	p = &cdb_component_dbs->segment_db_info[0];

	for ( i=0; i<seg_count; i++ )
	{
		int j;	
		
		segDesc = &ClusterDefn->segment_descriptors[i];
		
		Assert(	segDesc->db_count > 0 && segDesc->db_count <= 2 );
		
	    oldContext = MemoryContextSwitchTo(permanentContext);

		segDesc->db_descriptors = (SegmentDatabaseDescriptor *)
						palloc0( sizeof(SegmentDatabaseDescriptor) * 
								segDesc->db_count );

		MemoryContextSwitchTo(oldContext);
			
							
		for ( j=0; j<segDesc->db_count; j++ )
		{
			Assert( p->segindex == segDesc->segindex );
			segdbDesc = &segDesc->db_descriptors[j];
			segdbDesc->segment_descriptor = segDesc;
			segdbDesc->segment_database_info = p;
			segdbDesc->conn = NULL;		
			segdbDesc->thread = (pthread_t)NULL;
			segdbDesc->error_message = NULL;
			segdbDesc->cmdTuples = 0;
			segdbDesc->resultStatus = PGRES_COMMAND_OK;
			segdbDesc->cmdStatus = NULL;
			segdbDesc->segindex = p->segindex;
			
			p++;
		}									
		
	}
	
	return ClusterDefn;
}


/*

CdbComponentDatabaseInfo * getComponentDatabaseInfo( int segId, int instId )
{
	SegmentDatabaseDescriptor   *segdbDesc;
	SegmentDescriptor           *segDesc;		
	CdbComponentDatabases 		*cdb_component_dbs;
	
	
	segDesc = &CdbClusterDefinition->segment_descriptors[segId];
	
	if( instId < segDesc->db_count )
	{		
		segdbDesc = &segDesc->db_descriptors[instId]; 
		return segdbDesc->segment_database_info;	
		
	}
	else
		return NULL;
	
}*/

void *
getEntrySegDB(int *count)
{
    *count = cdb_component_dbs->total_entry_dbs;
    return cdb_component_dbs->entry_db_info;
}

