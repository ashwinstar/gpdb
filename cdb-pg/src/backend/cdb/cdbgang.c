/*-------------------------------------------------------------------------
 *
 * cdbgang.c
 *	  Query Executor Factory for gangs of QEs
 *
 * Copyright (c) 2005, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>                 /* getpid() */
#include <arpa/inet.h>              /* inet_ntoa() */
#include <pthread.h>

#include "libpq-fe.h"
#include "miscadmin.h"              /* MyDatabaseId */
#include "commands/dbcommands.h"    /* get_database_name() */

#include "cdb/cdbconn.h"            /* SegmentDatabaseDescriptor */
#include "cdb/cdbdisp.h"            /* cdbdisp_destroyDispatchResults() */
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"            /* me */
#include "cdb/cdbmotion.h"          /* CdbProcess */
#include "cdb/cdbtm.h"              /* discardDtxTransaction() */
#include "cdb/cdbutil.h"            /* CdbComponentDatabaseInfo */
#include "cdb/cdbvars.h"            /* Mpp_role, etc. */


/*
 *	thread_DoConnect is the thread proc used to perform the connection to one of the qExecs.
 */
static void *thread_DoConnect(void *arg);

static Gang *createGang(GangType type, int size, int content);

static void disconnectAndDestroyGang(Gang * gp);

static Gang *buildGangDefinition(GangType type, int size, int content);

static void cleanupGang(Gang * gp);

/*
 * Points to the result of getCdbComponentDatabases()
 */
static CdbComponentDatabases *cdb_component_dbs = NULL;
static bool mirroringNotConfigured = false;

/*
 * Parameter structure for the DoConnect threads
 */
typedef struct DoConnectParms
{
	/*
	 * db_count: The number of segdbs that this thread is responsible for
	 * connecting to.
	 * Equals the count of segdbDescPtrArray below.
	 */
	int			db_count;

	/*
	 * segdbDescPtrArray: Array of SegmentDatabaseDescriptor* 's that this thread is
	 * responsible for connecting to. Has size equal to db_count.
	 */
	SegmentDatabaseDescriptor **segdbDescPtrArray;

	/*
	 * The pthread_t thread handle.
	 */
	pthread_t	thread;
}	DoConnectParms;

/*
 * AddSegDBToConnThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to be connected to.
 * It figures out which thread will handle it, based on the setting of
 * mpp_connections_per_thread.
 */
static void addSegDBToConnThreadPool(DoConnectParms * ParmsAr, int *segdbCount,
						 SegmentDatabaseDescriptor * segdbDesc);


static char *myDatabaseName = NULL;


/*
 * creates a new gang by logging on a session to each segDB involved
 *
 */
Gang *
createGang(GangType type, int size, int content)
{
	Gang	   *newGangDefinition;
	MemoryContext oldContext;
	int			successful_connections = 0;

	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *q;
	int			i;
	DoConnectParms *doConnectParmsAr = NULL;
	int			threadCount = 0;
	int			max_threads;
	int			segdbs_in_thread_pool;
	int			segdb_count;

	Assert(size == 1 || size == getMPPSegmentCount());



	/*
	 * We can't allocate more segments than we know about
	 */
	Assert(size <= getMPPSegmentCount());


	/*
	 * MemoryContexts used in this routine.
	 */

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	if (myDatabaseName == NULL)
		myDatabaseName = get_database_name(MyDatabaseId);

	newGangDefinition = buildGangDefinition(type, size, content);

	if (type == GANGTYPE_MIRROR_WRITER && newGangDefinition == NULL)
	{
		MemoryContextSwitchTo(oldContext);
		return NULL;			/* System is not configured with mirrors */
	}

	Assert(newGangDefinition != NULL);


	/*
	 * Loop through the segment_database_descriptors items inside newGangDefinition.
	 * For each segdb , attempt to get
	 * a connection to the Postgres instance for that database.
	 * If the connection fails, mark it (locally) as unavailable
	 * If no database serving a segindex is available, fail the user's connection request.
	 */

	segdb_count = newGangDefinition->size;

	Assert(mpp_connections_per_thread > 0);
	Assert(segdb_count > 0);

	/*
	 * The most threads we could have is segdb_count / mpp_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / mpp_connections_per_thread.
	 * We allocate enough memory for this many DoConnectParms structures,
	 * even though we may not use them all.
	 */
	max_threads = 1 + (segdb_count - 1) / mpp_connections_per_thread;
	Assert(max_threads > 0);
	doConnectParmsAr = (DoConnectParms *)
		palloc0(max_threads * sizeof(DoConnectParms));
	segdbs_in_thread_pool = 0;

	/*
	 * In this loop, we check each segdb.  If it's marked as valid, and is not
	 * ourself, we prepare a thread Parm struct for connecting to it.
	 * If a segment has no valid segdbs, we recognize that here and give
	 * an error message.
	 */
	for (i = 0; i < segdb_count; i++)
	{


		/*
		 * Create the connection requests.	If we find a segment without a valid segdb
		 * we error out.  Also, if this segdb is invalid, we msut fail the connection.
		 */


		segdbDesc = &newGangDefinition->db_descriptors[i];
		/*
		 * q = segdbDesc->segment_database_info;
		 */

		/*
		 * This function handles putting the segdbDesc into the proper thread's Parms
		 * structure, based on mpp_connections_per_thread.
		 * It does this based on the value of segdbs_in_thread_pool,
		 * which it increments each time it is called.
		 */
		addSegDBToConnThreadPool(doConnectParmsAr, &segdbs_in_thread_pool,
								 segdbDesc);

	}

	/*
	 * Spin off the threads we've prepared.
	 */
	if (segdbs_in_thread_pool == 0)
		threadCount = 0;
	else
		threadCount = 1 + (segdbs_in_thread_pool - 1) / mpp_connections_per_thread;

	for (i = 0; i < threadCount; i++)
	{
		DoConnectParms *pParms = &doConnectParmsAr[i];

		elog(DEBUG2, "createGang Creating thread %d of %d for libpq connections",
			 i + 1, threadCount);

		pthread_create(&pParms->thread, NULL, thread_DoConnect, pParms);
	}

	/*
	 * wait for all of the DoConnect threads to complete.
	 */
	for (i = 0; i < threadCount; i++)
	{
		DoConnectParms *pParms = &doConnectParmsAr[i];

		elog(DEBUG2, "createGang Joining to thread %d of %d for libpq connections",
			 i + 1, threadCount);

		pthread_join(pParms->thread, NULL);
	}

	/*
	 * Free the memory allocated for the threadParms array
	 */
	if (doConnectParmsAr != NULL)
	{
		for (i = 0; i < threadCount; i++)
		{
			DoConnectParms *pParms = &doConnectParmsAr[i];

			pfree(pParms->segdbDescPtrArray);
			pParms->segdbDescPtrArray = NULL;
		}
		pfree(doConnectParmsAr);
		doConnectParmsAr = NULL;
	}

	/*
	 * For now, set all_valid_segdbs_connected to true.
	 * Reset to false if we get fail to connect to a valid segdb
	 */
	newGangDefinition->all_valid_segdbs_connected = true;

	/*
	 * In this loop, we check whether the connections were successful.
	 * If not, we recreate the error message with palloc and show it as
	 * a warning.
	 */
	for (i = 0; i < segdb_count; i++)
	{

		segdbDesc = &newGangDefinition->db_descriptors[i];
		q = segdbDesc->segment_database_info;

		/*
		 * check connection established or not,
		 * * if not, we may have to re-build this gang.
		 */
		if (segdbDesc->conn == NULL)
		{
			/*
			 * Set a warning for failed connections.  Complete failures
			 * are taken care of later.
			 */
			ereport(WARNING,
					(errcode(ERRCODE_WARNING),
                    errmsg("Unable to connect to segment database: "
                            "dbname=%s  %s",
                            myDatabaseName,
					        segdbDesc->error_message.data)));

            resetPQExpBuffer(&segdbDesc->error_message);
			
            newGangDefinition->all_valid_segdbs_connected = false;
		}
		else
		{
			/*
			 * We have a live connection!
			 */
			successful_connections++;
			/*
			 * segDesc->connected_count++;
			 */
		}


	}

	/*
	 * If no segdb for a segment got a successful
	 * connection, we error out.  We had to do this as a separate
	 * loop from the preceding one to make sure all error messages were
	 * palloced before potentially calling ereport.
	 */
	if (successful_connections != newGangDefinition->size &&
		((type != GANGTYPE_MIRROR_WRITER && mirroringNotConfigured) ||
		 !isFTSEnabled()))
	{
		for (i = 0; i < segdb_count; i++)
		{
			segdbDesc = &newGangDefinition->db_descriptors[i];

			if (segdbDesc->conn != NULL)
				continue;

			/*
			 * cleanup and rollback current DTM transaction
			 */
			discardDtxTransaction();

			/* clean the gang before error out */
			disconnectAndDestroyGang(newGangDefinition);

			ereport(FATAL,
					(errcode(ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION),
                     errmsg("createGang could not establish connection with any"
                            " database for segindex %d", i)));
		}
	}

	MemoryContextSwitchTo(oldContext);

	elog(DEBUG1, "createGang MPP segment databases: askedfor=%d, connected=%d",
		 size, successful_connections);
	elog(DEBUG1, "createGang MPP segments: defined = %d",
		 newGangDefinition->size);

	if (successful_connections != newGangDefinition->size && isFTSEnabled())
	{
		bool		errorOut;

		if (type == GANGTYPE_MIRROR_WRITER && successful_connections == 0)
			mirroringNotConfigured = true;

		errorOut = FtsHandleGangConnectionFailure(newGangDefinition->db_descriptors,
												  newGangDefinition->size);

		disconnectAndDestroyGang(newGangDefinition);

		if (errorOut)
			elog(ERROR, "MPP detects segment failures, system is reconnected");

		newGangDefinition = NULL;
	}
	return newGangDefinition;
}


/*
 *	buildGangDefinition: reads the MPP catalog tables and build a CdbComponentDatabases structure.
 *	It then converts this to a Gang structure and initializes
 *	all the non-connection related fields.
 */
Gang *
buildGangDefinition(GangType type, int size, int content)
{
	Gang	   *newGangDefinition = NULL;

	/*
	 * SegmentDescriptor			*segDesc;
	 */
	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *p;

	int			seg_count = 0;
	int			last_segindex = -1;
	int			i;

	MemoryContext oldContext;
	MemoryContext permanentContext = TopMemoryContext;

	/*
	 * Get the structure representing the configured segment databases.
	 * This structure is fetched into a permanent memory context -- probably the TopMemoryContext.	We
	 * also allocate space for and partially construct the data structure
	 * pointed to by ClusterDefn and headed by CdbClusterDescriptor.
	 */
	oldContext = MemoryContextSwitchTo(permanentContext);

	newGangDefinition = (Gang *) palloc0(sizeof(Gang));

	newGangDefinition->type = type;
    newGangDefinition->dispatchResults = NULL;

	switch (type)
	{
		case GANGTYPE_PRIMARY_WRITER:
			elog(DEBUG4, "building Gang.  Gang is primary writer");
			break;

		case GANGTYPE_MIRROR_WRITER:
			elog(DEBUG4, "building Gang.  Gang is mirror writer");
			break;

		case GANGTYPE_READER:
			/*
			 * Must be size 1 or size numsegments
			 */
			/*
			 * If size 1, then content tells us what?
			 */
			elog(DEBUG4,
				 "building Gang.  Gang is reader size %d for content %d", size, content);
			break;

		case GANGTYPE_UNALLOCATED:
			Assert(FALSE);
			newGangDefinition = NULL;
			break;
	}

	newGangDefinition->all_valid_segdbs_connected = false;

	/* retry transient failed dbs */
	if (type == GANGTYPE_PRIMARY_WRITER)
		FtsEvaluateTransientState();

	cdb_component_dbs = getCdbComponentDatabases();

	if (cdb_component_dbs == NULL || cdb_component_dbs->total_segments <= 0
		|| cdb_component_dbs->total_segment_dbs <= 0)
	{
		MemoryContextSwitchTo(oldContext);
		elog(ERROR, "building Gang:  MPP schema not populated");
		return NULL;
	}

    /* if mirroring is not configured */
    if (cdb_component_dbs->total_segment_dbs == cdb_component_dbs->total_segments)
    {
        disableFTS();
        mirroringNotConfigured = true;
    }

	newGangDefinition->segdb_count = cdb_component_dbs->total_segments;
	/*
	 * if (type == GANGTYPE_READER && size == 1)
	 * newGangDefinition->size = size;
	 * else
	 * {
	 * Assert(size == newGangDefinition->segdb_count);
	 * }
	 */
	Assert(newGangDefinition->segdb_count > 0);


	newGangDefinition->db_descriptors =
		(SegmentDatabaseDescriptor *) palloc0(cdb_component_dbs->
											  total_segments *
											  sizeof
											  (SegmentDatabaseDescriptor));



	MemoryContextSwitchTo(oldContext);


	Assert(cdb_component_dbs->total_segment_dbs > 0);



	/*
	 * Pass through each segment database in segment_dbs
	 * and Count up databases per segment_index.  Use this to initialize
	 * the fields in ClusterDefn->segment_descriptors
	 */


	/*
	 * We loop through the segment_db_info.  Each item has a segindex.
	 * They are sorted by segindex, and there can be > 1 segment_db_info for
	 * a given segindex (currently, there can be 1 or 2)
	 */

	for (i = 0; i < cdb_component_dbs->total_segment_dbs; i++)
	{
		p = &cdb_component_dbs->segment_db_info[i];
		if (i == 0 || (p->segindex != last_segindex))
		{
			int			j = i;

			/*
			 * We have a new segindex.	Advance segDesc, and set count to 1.
			 */
			/*
			 * segDesc++;
			 * segDesc->segindex = p->segindex;
			 * segDesc->db_count = 1;
			 * segDesc->error_message = NULL;
			 * segDesc->query_db = NULL;
			 * segDesc->sql = NULL;
			 */

			if (p->definedprimary && type == GANGTYPE_MIRROR_WRITER)
			{
				j++;
				if (cdb_component_dbs->segment_db_info[j].segindex ==
					p->segindex
					&& !cdb_component_dbs->segment_db_info[j].definedprimary)
				{
					p = &cdb_component_dbs->segment_db_info[j];
					i = j;
				}
				else
				{
					return NULL;
				}
			}
			else if ((!p->definedprimary && !p->isprimary))
			{
				j++;
				if (cdb_component_dbs->segment_db_info[j].segindex ==
					p->segindex
					&& cdb_component_dbs->segment_db_info[j].definedprimary)
				{
					/*
					 * Do we want to change p ?
					 */
					p = &cdb_component_dbs->segment_db_info[j];
					i = j;
				}
				else
				{
					elog(PANIC, "building Gang:  No primary segdb found, both primary and mirror segdb could be down !!!!");
					continue;
				}
			}


			if (p->valid)
			{
				if (type == GANGTYPE_MIRROR_WRITER && p->isprimary)
				{
					/*
					 * this segdb has been used by primary gang, can not be used
					 * * by mirror gang again
					 */
					CondSetMPPReadonly();
				}
				else if (type == GANGTYPE_READER && size == 1 &&
						 content != p->segindex)
				{
					last_segindex = p->segindex;
					continue;
				}
				else
				{
					segdbDesc = &newGangDefinition->db_descriptors[seg_count];

                    cdbconn_initSegmentDescriptor(segdbDesc, p);

					seg_count++;
					if (type == GANGTYPE_READER && size == 1)
						/* we are done */
						break;
				}
				last_segindex = p->segindex;
			}
			else
			{
				CondSetMPPReadonly();
			}
		}
		else
		{
			/*
			 * We have another segdb for the same segindex.  Bump up the count.
			 * It should never go above 2, since we can only have 1 secondary
			 * segdb in the current design.
			 */
			/*
			 * segDesc->db_count++;
			 * Assert( segDesc->db_count <= 2 );
			 */
		}
	}

	/*
	 * Assert( seg_count == cdb_component_dbs->total_segments );
	 */

	if (type == GANGTYPE_PRIMARY_WRITER
		&& seg_count != cdb_component_dbs->total_segments)
	{
		elog(PANIC, "Not all primary segment instances are active and connected");
	}
	else if (type == GANGTYPE_MIRROR_WRITER)
	{
		/* setup mirroring */
		if (seg_count == 0)
		{
			disableFTS();
			mirroringNotConfigured = true;
			return NULL;
		}
	}

	newGangDefinition->size = seg_count;
	return newGangDefinition;
}


/*
 * AddSegDBToConnThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to be connected to.
 * It figures out which thread will handle it, based on the setting of
 * mpp_connections_per_thread.
 */
void
addSegDBToConnThreadPool(DoConnectParms * ParmsAr, int *segdbCount,
						 SegmentDatabaseDescriptor * segdbDesc)
{
	DoConnectParms *pParms;
	int			ParmsIndex;

	Assert(segdbCount != NULL);

	/*
	 * The proper index into the DoConnectParms array is computed, based on
	 * having mpp_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % mpp_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	ParmsIndex = (*segdbCount) / mpp_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];

	if ((*segdbCount) % mpp_connections_per_thread == 0)
	{
		Assert(mpp_connections_per_thread > 0);
		pParms->segdbDescPtrArray =
			(SegmentDatabaseDescriptor **) palloc0(mpp_connections_per_thread *
												   sizeof
												   (SegmentDatabaseDescriptor
													*));
		memset(&pParms->thread, 0, sizeof(pthread_t));
		pParms->db_count = 0;
	}

	/*
	 * Just add to the end of the used portion of the segdbDescPtrArray
	 * and bump the count of members
	 */
	pParms->segdbDescPtrArray[pParms->db_count++] = segdbDesc;

	/*
	 * Bump the count of segdbs processed
	 */
	(*segdbCount)++;
}

/*
 *	thread_DoConnect is the thread proc used to perform the connection to one of the qExecs.
 */
static void *
thread_DoConnect(void *arg)
{
	DoConnectParms *pParms = (DoConnectParms *) arg;
	SegmentDatabaseDescriptor **segdbDescPtrArray = pParms->segdbDescPtrArray;
	int			db_count = pParms->db_count;

	int			i;

	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *q;

	/*
	 * The pParms contains an array of SegmentDatabaseDescriptors
	 * to connect to.
	 */
	for (i = 0; i < db_count; i++)
	{
		PQExpBufferData buffer;

		segdbDesc = segdbDescPtrArray[i];
		q = segdbDesc->segment_database_info;

		/*
		 * We use PQExpBufferData instead of StringInfoData
		 * because the former uses malloc, the latter palloc.
		 * We are in a thread, and we CANNOT use palloc since it's not
		 * thread safe.  We cannot call elog or ereport either for the
		 * same reason.
		 */

		initPQExpBuffer(&buffer);

		/*
		 * Build the connection string
		 */
		appendPQExpBuffer(&buffer,
						  "options='-c mpp_session_role=EXECUTE -c mpp_segment=%d -c mpp_max_packet_size=%d' ",
						  q->segindex, Mpp_max_packet_size);

		if (cdbconn_doConnect(segdbDesc, myDatabaseName, buffer.data))
		{
            if (segdbDesc->motionListener == -1)
            {
                appendPQExpBuffer(&segdbDesc->error_message,
                    "(%s) Connection does not contain a valid motion listener port.\n",
                    segdbDesc->whoami);
                PQfinish(segdbDesc->conn);
                segdbDesc->conn = NULL;
            }
		}

		free(buffer.data);
	}

	return (NULL);
}                               /* thread_DoConnect */


/*
 * This is the main routine.   It allocates a gang to a query.
 * If we have a previously created but unallocated gang lying around, we use that.
 * otherwise, we create a new gang
 *
 */

static List *allocatedReaderGangsN = NIL;
static List *availableReaderGangsN = NIL;
static List *allocatedReaderGangs1 = NIL;
static List *availableReaderGangs1 = NIL;
static Gang *primaryWriterGang = NULL;
static Gang *mirrorWriterGang = NULL;

Gang *
allocateGang(GangType type, int size, int content)
{
	/*
	 * First, we look for an unallocated but created gang of the right type
	 * if it exists, we return it.
	 * Else, we create a new gang
	 */

	MemoryContext oldContext;

	Gang	   *gp = NULL;

	Assert(Mpp_role == MPP_ROLE_DISPATCH);

	if (Mpp_role != MPP_ROLE_DISPATCH)
		return NULL;

	elog(DEBUG4, "allocateGang: allocatedReaderGangsN %d, availableReaderGangsN %d",
		 list_length(allocatedReaderGangsN),
		 list_length(availableReaderGangsN));

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

    /* Make sure CreateGang will use the current database name. */
    free(myDatabaseName);
    myDatabaseName = NULL;
    
    switch (type)
	{
		case GANGTYPE_PRIMARY_WRITER:
			if (primaryWriterGang == NULL)
			{
				/*
				 * try twice for creating gang
				 */
				gp = primaryWriterGang = createGang(type, size, content);
				if (gp == NULL)
					gp = primaryWriterGang = createGang(type, size, content);
			}
			else
				gp = primaryWriterGang;
			break;

		case GANGTYPE_MIRROR_WRITER:
			if (mirrorWriterGang == NULL && !mirroringNotConfigured)
			{
				/*
				 * try twice for creating gang
				 */
				gp = mirrorWriterGang = createGang(type, size, content);
				if (gp == NULL && !mirroringNotConfigured)
					gp = mirrorWriterGang = createGang(type, size, content);
			}
			else
				gp = mirrorWriterGang;
			break;

		case GANGTYPE_READER:
			/*
			 * Must be size 1 or size numsegments
			 * If size 1, then content tells us which segment we want
			 */
			elog(DEBUG4, "building Gang.  Gang is reader size %d", size);
			if (size != 1)
			{

				if (availableReaderGangsN != NIL)		/* There are gangs
														 * already created */
				{
					elog(DEBUG4, "reusing an old gang");

					gp = linitial(availableReaderGangsN);
					Assert(gp != NULL);
					Assert(gp->type == type && gp->size > 1);

					elog(DEBUG4, "deleting it from the old list ");
					availableReaderGangsN =
						list_delete_first(availableReaderGangsN);

				}
				else
					/*
					 * no pre-created gang exists
					 */
				{
					elog(DEBUG4, "creating a new gang");
					gp = createGang(type, size, content);
					/* try again */
					if (gp == NULL)
						gp = createGang(type, size, content);
				}
				Assert(gp != NULL);
				if (gp == NULL)
					elog(ERROR, "No reader gang allocated");

				if (allocatedReaderGangsN != NIL)
					elog(DEBUG4, "allocatedReaderGangsN Exists");
				else
					elog(DEBUG4, "allocatedReaderGangsN is NIL");

				elog(DEBUG4, "adding it to the allocated gang list");
				allocatedReaderGangsN = lappend(allocatedReaderGangsN, gp);

			}
			else
				/*
				 * size 1 gangs
				 */
			{

				if (availableReaderGangs1 != NIL)		/* There are gangs
														 * already created */
				{
					elog(DEBUG4, "reusing an old gang?");

					gp = linitial(availableReaderGangs1);

					Assert(gp->type == type && gp->size == 1);
					/*
					 * does it have the right content ?
					 */
					if (gp->db_descriptors[0].segindex == content)
					{
						elog(DEBUG4, "yes, reusing an old gang");
						availableReaderGangs1 =
							list_delete_first(availableReaderGangs1);
					}
					else
					{
						elog(DEBUG4, "no available gang with right content");
						gp = NULL;
					}

				}
				if (gp == NULL) /* no pre-created gang exists */
				{
					elog(DEBUG4, "creating a new 1 gang for content %d", content);
					gp = createGang(type, size, content);
					/* try again */
					if (gp->size == 0)
						gp = createGang(type, size, content);
				}
				if (gp == NULL)
					elog(ERROR, "No reader gang allocated");
				allocatedReaderGangs1 = lappend(allocatedReaderGangs1, gp);

			}

			break;

		default:
			elog(NOTICE, "problem in allocateGang");
			break;
	}

    elog(DEBUG4,
		 "allocateGang on return: allocatedReaderGangsN %d, availableReaderGangsN %d",
		 list_length(allocatedReaderGangsN),
		 list_length(availableReaderGangsN));


	/*
	 * now we enroll the gang in the current global transaction?
	 */

	MemoryContextSwitchTo(oldContext);

	return gp;
}

struct SegmentDatabaseDescriptor *
getSegmentDescriptorFromGang(const Gang * gp, int seg)
{

	int			i;

	if (gp == NULL)
		return NULL;
		
	for (i = 0; i < gp->size; i++)
	{
		if (gp->db_descriptors[i].segindex == seg)
			return &(gp->db_descriptors[i]);
	}

	return NULL;
}

List *
getCdbProcessList(Gang * gang)
{
	int			i;
	List	   *list = NIL;


	Assert(Mpp_role == MPP_ROLE_DISPATCH && Mpp_is_primary);
	Assert(gang != NULL);
	elog(DEBUG4, "getCdbProcessList, gangtype = %d, gangsize = %d", gang->type, gang->size);

	if (gang != NULL && gang->type != GANGTYPE_UNALLOCATED)
	{
		CdbComponentDatabaseInfo *qeinfo;
		struct hostent *theHost;
		bool		primary;
		int			k = 0;
		int			listsize = 0;

		/*
		 * Assert( gang->size == 1 || gang->size == getMPPSegmentCount() );
		 */

		primary = (gang->type != GANGTYPE_MIRROR_WRITER);

		for (i = 0; i < gang->size; i++)
		{
			SegmentDatabaseDescriptor *segdbDesc;
			CdbProcess *process = (CdbProcess *) makeNode(CdbProcess);


			segdbDesc = &gang->db_descriptors[i];
			qeinfo = segdbDesc->segment_database_info;

			if (qeinfo == NULL)
			{
				elog(ERROR, "getCdbProcessList: a required QE is unexpectedly unavailable");
			}

			theHost = gethostbyname(qeinfo->hostname);
			if (theHost == NULL)
			{
				elog(FATAL, "getCdbProcessList: gethostbyname() failed for host '%s'", qeinfo->hostname);
			}
			
			/*
			 * We want the n-th element of the list to be the segDB that handled content n.
			 * And if no segDb is available (down mirror), we want it to be null.
			 *
			 * But the gang structure element n is not necessarily the guy who handles content n,
			 * so we need to skip some slots.
			 *
			 *
			 * We don't do this for reader 1-gangs
			 *
			 */

			if (gang->type != GANGTYPE_READER || gang->size > 1)
				while (segdbDesc->segindex > listsize)
				{
					list = lappend(list, NULL);
					listsize++;
				}


			process->listenerAddr = pstrdup(inet_ntoa(*(struct in_addr *) (theHost->h_addr_list[0])));
			process->listenerPort = segdbDesc->motionListener;
			process->pid = PQbackendPID(segdbDesc->conn);
			process->contentid = segdbDesc->segindex;
			process->routeid = k++;

			elog(DEBUG1, "getCdbProcessesList --- %s, port %d, content %d, routeid %d",
				 process->listenerAddr, process->listenerPort,
				 process->contentid, process->routeid);

			list = lappend(list, process);
			listsize++;
		}

		if ((gang->type != GANGTYPE_READER || gang->size > 1) && listsize < getMPPSegmentCount())
		{
			if (gang->type == GANGTYPE_PRIMARY_WRITER)
				elog(ERROR, "Primary gang smaller than number of segments");

			while (listsize < getMPPSegmentCount())
			{
				list = lappend(list, NULL);
				listsize++;
			}
		}
		Assert(listsize == 1 || listsize == getMPPSegmentCount());
	}

	return list;
}

/*
 * getCdbProcessForQD:	Manufacture a CdbProcess representing the QD,
 * as if it were a worker from the executor factory.
 *
 * NOTE: Does not support multiple (mirrored) QDs.
 */
List *
getCdbProcessesForQD(int isPrimary)
{
	List	   *list = NIL;

	CdbComponentDatabases *cdb_component_dbs;
	CdbComponentDatabaseInfo *qdinfo;
	struct hostent *theHost;
	CdbProcess *proc;


	Assert(Mpp_role == MPP_ROLE_DISPATCH && Mpp_is_primary);
	if (!isPrimary)
	{
		elog(FATAL, "getCdbProcessesForQD: unsupported request for QD mirror process");
	}

	cdb_component_dbs = getCdbComponentDatabases();		/* from cdbutil.h */
	qdinfo = &(cdb_component_dbs->entry_db_info[0]);

	Assert(qdinfo->segindex == -1 && qdinfo->isprimary);

	theHost = gethostbyname(qdinfo->hostname);

	if (theHost == NULL)
	{
		elog(FATAL, "gethostbyname() failed for host '%s'", qdinfo->hostname);
	}

	proc = makeNode(CdbProcess);
	proc->listenerAddr =
		pstrdup(inet_ntoa(*(struct in_addr *) (theHost->h_addr_list[0])));
	proc->listenerPort = Mpp_listener_port;
	proc->pid = getpid();
	proc->contentid = -1;
	proc->routeid = 0;

	/*
	 * freeCdbComponentDatabases(cdb_component_dbs);
	 */


	list = lappend(list, proc);
	return list;
}


void
cleanupGang(Gang * gp)
{
	int			i;

	if (gp == NULL)
		return;

	elog(DEBUG5, "cleanupGang");
	/*
	 * Loop through the segment_database_descriptors array and, for each
	 * SegmentDatabaseDescriptor:
	 *	   1) discard the query results (if any)
	 */
	for (i = 0; i < gp->size; i++)
	{


		SegmentDatabaseDescriptor *segdbDesc = &(gp->db_descriptors[i]);

		/*
		 * TODO: Perform cancellation of "still running" queries
		 */

		if (segdbDesc->conn && PQstatus(segdbDesc->conn) == CONNECTION_OK)
		{

			PGresult   *pRes;
			int			retry = 0;
			while (NULL != (pRes = PQgetResult(segdbDesc->conn)))
			{
                ExecStatusType  stat = PQresultStatus(pRes);

                elog(LOG, "(%s) Leftover result at freeGang time: %s %s",
                     segdbDesc->whoami,
                     PQresStatus(stat),
                     PQerrorMessage(segdbDesc->conn));

				PQclear(pRes);
				if (stat == PGRES_FATAL_ERROR)
					break;
				if (stat == PGRES_BAD_RESPONSE)
					break;
				retry++;
				if (retry > 20)
				{
					elog(LOG, "freeGang called when a gang member is still busy, and waiting didn't help, trying to destroy the gang");
					disconnectAndDestroyGang(gp);
					elog(FATAL, "freeGang called when a gang member is still busy.  Fatal error.");
                    return;
				}
				
			};

		}

        /* QE is no longer associated with a slice. */
        cdbconn_setSliceIndex(segdbDesc, -1);

	}

    /*
     * Free CdbDispatchResults.
     */
    if (gp->dispatchResults)
    {
        StringInfoData msg;

        initStringInfo(&msg);
        if (!cdbdisp_dumpGangResults(gp, &msg, false))
        {
            elog(LOG, "Leftover result at freeGang time: %s", msg.data);
        }
        pfree(msg.data);

        cdbdisp_destroyGangResults(gp);
    }

	elog(DEBUG5, "cleanupGang done");

}


void
disconnectAndDestroyGang(Gang * gp)
{
	int			i;

	if (gp == NULL)
		return;

	elog(DEBUG5, "disconnectAndDestroyGang");
	/*
	 * Loop through the segment_database_descriptors array and, for each
	 * SegmentDatabaseDescriptor:
	 *	   1) discard the query results (if any),
	 *	   2) disconnect the session, and
	 *	   3) discard any connection error message.
	 */
	for (i = 0; i < gp->size; i++)
	{


		SegmentDatabaseDescriptor *segdbDesc = &(gp->db_descriptors[i]);

		/*
		 * TODO: Perform cancellation of "still running" queries
		 */

		if (segdbDesc->conn)
		{
			PGTransactionStatusType status =
			PQtransactionStatus(segdbDesc->conn);

			elog(DEBUG2,
				 "Finishing connection with %s; transaction status %d",
				 segdbDesc->whoami, status);
			PQfinish(segdbDesc->conn);

			segdbDesc->conn = NULL;
		}

        /* Free memory owned by the segdbDesc. */
        cdbconn_termSegmentDescriptor(segdbDesc);
	}


    /*
     * Free CdbDispatchResults.
     */
    cdbdisp_destroyGangResults(gp);

	/*
	 * Discard the segment array and the cluster descriptor
	 */
	pfree(gp->db_descriptors);
	gp->db_descriptors = NULL;
	gp->size = 0;
	pfree(gp);

	/*
	 * this is confusing, gp is local variable, no need to null it. gp = NULL;
	 */
	elog(DEBUG5, "disconnectAndDestroyGang done");


}


void
freeGangs(void)
{
	MemoryContext oldContext;

	if (Mpp_role != MPP_ROLE_DISPATCH)
		return;

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	elog(DEBUG2, "freeGangs: allocatedReaderGangsN %d, availableReaderGangsN %d",
	 list_length(allocatedReaderGangsN), list_length(availableReaderGangsN));

	cleanupGang(primaryWriterGang);
	cleanupGang(mirrorWriterGang);

	if (allocatedReaderGangsN != NIL)
	{
		Gang	   *gp = linitial(allocatedReaderGangsN);

		Assert(gp != NULL);

		while (gp != NULL)
		{
			elog(DEBUG4, "Returning an N gang to the available list");
			cleanupGang(gp);
			availableReaderGangsN = lappend(availableReaderGangsN, gp);
			allocatedReaderGangsN = list_delete_first(allocatedReaderGangsN);
			if (allocatedReaderGangsN != NIL)
				gp = linitial(allocatedReaderGangsN);
			else
				gp = NULL;
		}
	}
	if (allocatedReaderGangsN != NIL)
		elog(NOTICE, "allocatedReaderGangN didn't get freed!");

	if (allocatedReaderGangs1 != NIL)
	{
		Gang	   *gp = linitial(allocatedReaderGangs1);

		while (gp != NULL)
		{
			elog(DEBUG4, "Returning a 1 gang to the available list");
			cleanupGang(gp);
			availableReaderGangs1 = lappend(availableReaderGangs1, gp);
			allocatedReaderGangs1 = list_delete_first(allocatedReaderGangs1);
			if (allocatedReaderGangs1 != NIL)
				gp = linitial(allocatedReaderGangs1);
			else
				gp = NULL;
		}
	}

	MemoryContextSwitchTo(oldContext);

	elog(DEBUG2, "freeGangs done: allocatedReaderGangsN %d, availableReaderGangsN %d",
		 list_length(allocatedReaderGangsN),
		 list_length(availableReaderGangsN));

}


void
disconnectAndDestroyAllGangs(void)
{
	Gang	   *gp;

	elog(DEBUG5, "disconnectAndDestroyAllGangs");

    if (allocatedReaderGangsN != NIL)
	{
		gp = linitial(allocatedReaderGangsN);
		while (gp != NULL)
		{
			disconnectAndDestroyGang(gp);
			allocatedReaderGangsN = list_delete_first(allocatedReaderGangsN);
			if (allocatedReaderGangsN != NIL)
				gp = linitial(allocatedReaderGangsN);
			else
				gp = NULL;
		}
	}
	if (availableReaderGangsN != NIL)
	{
		gp = linitial(availableReaderGangsN);
		while (gp != NULL)
		{
			disconnectAndDestroyGang(gp);
			availableReaderGangsN = list_delete_first(availableReaderGangsN);
			if (availableReaderGangsN != NIL)
				gp = linitial(availableReaderGangsN);
			else
				gp = NULL;
		}
	}
	if (allocatedReaderGangs1 != NIL)
	{
		gp = linitial(allocatedReaderGangs1);
		while (gp != NULL)
		{
			disconnectAndDestroyGang(gp);
			allocatedReaderGangs1 = list_delete_first(allocatedReaderGangs1);
			if (allocatedReaderGangs1 != NIL)
				gp = linitial(allocatedReaderGangs1);
			else
				gp = NULL;
		}
	}
	if (availableReaderGangs1 != NIL)
	{
		gp = linitial(availableReaderGangs1);
		while (gp != NULL)
		{
			disconnectAndDestroyGang(gp);
			availableReaderGangsN = list_delete_first(availableReaderGangs1);
			if (availableReaderGangs1 != NIL)
				gp = linitial(availableReaderGangs1);
			else
				gp = NULL;
		}
	}
	allocatedReaderGangsN = NIL;
	availableReaderGangsN = NIL;
	allocatedReaderGangs1 = NIL;
	availableReaderGangs1 = NIL;

	disconnectAndDestroyGang(primaryWriterGang);
	primaryWriterGang = NULL;
	disconnectAndDestroyGang(mirrorWriterGang);
	mirrorWriterGang = NULL;

    elog(DEBUG5, "disconnectAndDestroyAllGangs done");
}


bool
reconnectSegDB(void *p_failedDB)
{
	int			segdbs_in_thread_pool = 0;
	bool		ret = true;
	DoConnectParms *doConnectParmsAr;
	SegmentDatabaseDescriptor *failedDB =
	(SegmentDatabaseDescriptor *) p_failedDB;
	PGconn	   *orig_conn = failedDB->conn;

	failedDB->conn = NULL;

	doConnectParmsAr = (DoConnectParms *) palloc0(sizeof(DoConnectParms));

	addSegDBToConnThreadPool(doConnectParmsAr, &segdbs_in_thread_pool,
							 failedDB);

	/*
	 * elog(DEBUG2, "reconnectSegDB reconnect segDB: %d", failedDB->segment_database_info->dbid);
	 */
	thread_DoConnect((void *) doConnectParmsAr);

	/*
	 * release connectParms
	 */
	pfree(doConnectParmsAr->segdbDescPtrArray);
	pfree(doConnectParmsAr);

	/*
	 * check new connection
	 */
	if (PQstatus(failedDB->conn) == CONNECTION_BAD)
	{
        elog(LOG, "FTS: dbname %s reconnect failed. %s",
             orig_conn->dbName,
             failedDB->error_message.data);

		PQfinish(failedDB->conn);
		ret = false;

		/*
		 * set the orginal db back, otherwise it is set to NULL which may cause
		 * * dispatch code to fail.
		 */
		if (!isFTSEnabled())
			failedDB->conn = orig_conn;
		else
			PQfinish(orig_conn);
	}
	else
	{
		elog(LOG, "FTS: segdb %d reconnect ok", failedDB->segment_database_info->dbid);
	}

	return ret;
}

/* set segdb states, called by FtsReConfigureMPP and lock should be held there */
void
detectFailedConnections(void)
{
	int			i;

	/*
	 * check primary gang
	 */
	if (primaryWriterGang != NULL)
	{
		for (i = 0; i < primaryWriterGang->size; i++)
		{
			SegmentDatabaseDescriptor *segdbDesc =
			&(primaryWriterGang->db_descriptors[i]);

			if (!FtsTestConnection(segdbDesc->segment_database_info, NULL))
				markSegDBStatus(segdbDesc->segment_database_info->dbid, false);
		}
	}

	/*
	 * check mirrored gang
	 */
	if (mirrorWriterGang != NULL)
	{
		for (i = 0; i < mirrorWriterGang->size; i++)
		{
			SegmentDatabaseDescriptor *segdbDesc =
			&(mirrorWriterGang->db_descriptors[i]);

			if (!FtsTestConnection(segdbDesc->segment_database_info, NULL))
				markSegDBStatus(segdbDesc->segment_database_info->dbid, false);
		}
	}

}

CdbComponentDatabases *
getComponentDatabases(void)
{
	return (cdb_component_dbs == NULL) ? getCdbComponentDatabases() : cdb_component_dbs;
}

bool
isGangExisted(void)
{
	return (primaryWriterGang != NULL ||
			mirrorWriterGang != NULL ||
			allocatedReaderGangsN != NIL ||
			availableReaderGangsN != NIL ||
			allocatedReaderGangs1 != NIL ||
			availableReaderGangs1 != NIL);
}


