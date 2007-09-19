/*-------------------------------------------------------------------------
 *
 * cdbgang.c
 *	  Query Executor Factory for gangs of QEs
 *
 * Copyright (c) 2005-2007, greenplum inc
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>				/* getpid() */
#include <pthread.h>
#include <limits.h>

#include "libpq-fe.h"
#include "miscadmin.h"			/* MyDatabaseId */
#include "storage/proc.h"		/* MyProc */
#include "storage/ipc.h"
#include "utils/memutils.h"

#include "catalog/namespace.h"
#include "commands/variable.h"
#include "nodes/execnodes.h"    /* CdbProcess, Slice, SliceTable */
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"

extern int	CommitDelay;
extern int	CommitSiblings;
extern char *default_tablespace;

#include "cdb/cdbconn.h"		/* SegmentDatabaseDescriptor */
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"		/* me */
#include "cdb/cdbtm.h"			/* discardDtxTransaction() */
#include "cdb/cdbutil.h"		/* CdbComponentDatabaseInfo */
#include "cdb/cdbvars.h"		/* Gp_role, etc. */
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/libpq-be.h"
#include "libpq/ip.h"


/*
 *	thread_DoConnect is the thread proc used to perform the connection to one of the qExecs.
 */
static void *thread_DoConnect(void *arg);
static void build_gpqeid_params(PQExpBuffer buf);
static void build_gpdaid_params(PQExpBuffer buf);

static Gang *createGang(GangType type, int gang_id, int size, int content, char *portal_name);

static void disconnectAndDestroyGang(Gang * gp);
static void disconnectAndDestroyAllReaderGangs(void);
static void disconnectAndDestroyDispatchAgents(void);

static Gang *buildGangDefinition(GangType type, int gang_id, int size, int content, char *portal_name);
static Gang *buildGangDefinitionFromConninfo(GpSegmentConnectionInfoSet * conninfo);


static bool isTargetPortal(char *p1, char *p2);

static void addOptions(PQExpBufferData * buffer, bool iswriter, int segindex, bool isprimary, bool i_am_superuser);


/*
 * Points to the result of getCdbComponentDatabases()
 */
static CdbComponentDatabases *cdb_component_dbs = NULL;
static bool mirroringNotConfigured = false;

static int largest_gangsize = 0;

static MemoryContext GangContext = NULL;

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

	/* type of gang. */
	GangType	type;
	
	bool		i_am_superuser;

	/*
	 * The pthread_t thread handle.
	 */
	pthread_t	thread;
}	DoConnectParms;

/*
 * AddSegDBToConnThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to be connected to.
 * It figures out which thread will handle it, based on the setting of
 * gp_connections_per_thread.
 */
static void addSegDBToConnThreadPool(DoConnectParms * ParmsAr, int *segdbCount,
						 SegmentDatabaseDescriptor * segdbDesc);
 

static DispatchAgentSet * dispatchagents = NULL;

int numDispatchAgents(void)
{
	if (dispatchagents)
		return dispatchagents->size;
	else
		return 0;
}

int largestGangsize(void) 
{
	return largest_gangsize;
}
/*
 * create Dispatch Agents for this session
 */
static DispatchAgentSet *
createDispatchAgents(void)
{
	MemoryContext oldContext;

	CdbComponentDatabaseInfo *q;

	PQExpBufferData buffer;
	int i, j;
	char lasthost[255];
	
	/*
	 * Not a lot of thought has gone into tuning this.
	 * If we were just dispatching non-sliced plans, we'd want to limit
	 * the number of dispatch agents to about sqrt(getgpsegmentCount()) 
	 * to balance the load, but since we do have multi-slice plans, it may be
	 * useful to have as many as 1 dispatch agent per host.
	 * 
	 */
	
	int			size = getgpsegmentCount()/10;  /* 1/10 the number of segments */
	if (size < 2)				/* on small systems, use two dispatch agents */
		size = 2;
	
	if (getgpsegmentCount()<=1)	/* some developers use a single segment, I think */
		size = 1;

	elog(DEBUG1,"createDispatchAgents");
	
	/*
	 * Perhaps someday we will have a multi-level tree of dispatch agents, but right now
	 * we only allow the QD to create dispatch agents
	 */
	Assert(Gp_role == GP_ROLE_DISPATCH);
	/*
	 * We can't allocate more dispatch agents than segments than we know about
	 */
	Assert(size > 0 && size <= getgpsegmentCount());

	/*
	 * MemoryContexts used in this routine.
	 */
	if (GangContext == NULL)
	{ 
		GangContext = AllocSetContextCreate(TopMemoryContext,
											 "Gang Context",
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
	}

	oldContext = MemoryContextSwitchTo(GangContext);
	
	cdb_component_dbs = getCdbComponentDatabases();

	if (cdb_component_dbs == NULL || cdb_component_dbs->total_segments <= 0
		|| cdb_component_dbs->total_segment_dbs <= 0)
	{
		elog(ERROR, "create Dispatch Agent:  GP schema not populated");
		return NULL;
	}

	/*
	 * for now, grab a few random postmasters out of the segDBs, and create a 
	 * dispatch agent there.
	 * 
	 * // q = &cdb_component_dbs->entry_db_info[0]; // to test on entry db
	 * 
	 */
	
	dispatchagents = palloc0(sizeof(DispatchAgentSet));
	dispatchagents->size = 0;
	dispatchagents->db_descriptors = (SegmentDatabaseDescriptor *) palloc0(size * sizeof(SegmentDatabaseDescriptor));
	
	
	j = 0;	/* Starting point for the search */
	lasthost[0] = '\0';
	
	for (i = 0; i < size; i++)
	{
		/*
		 * Find a valid primary postmaster.  
		 * 
		 * On a different host from any other dispatchAgent
		 * we have created in this loop
		 * 
		 * To do this, we assume the normal way gpinitsystem sets
		 * up things:  The first segment on each host has the same port
		 * 
		 * hostname is not enough, because we use different hostnames
		 * for each lan adapter on the system.
		 */
		q = &cdb_component_dbs->segment_db_info[j++];
		while ((!q->isprimary  || !q->valid || 
				q->port != cdb_component_dbs->segment_db_info[0].port || 
				strcmp(lasthost,q->hostname)==0) && 
					j < cdb_component_dbs->total_segment_dbs)
			q = &cdb_component_dbs->segment_db_info[j++];
		
		if (j >= cdb_component_dbs->total_segment_dbs)
			break;
	 
		if (strcmp(q->hostname,"localhost")!=0)
			strcpy(lasthost, q->hostname);
		
		elog(LOG,"Using dispatch agent on %s %d segindex %d",q->hostname,q->port,q->segindex);
		 
		cdbconn_initSegmentDescriptor(&dispatchagents->db_descriptors[i],
				  q);
		
		dispatchagents->size++;
		//dispatchagents->db_descriptors[i]->segindex = -2;
	
	
		initPQExpBuffer(&buffer);
		
		/*
		 * Build the connection string
		 */
		build_gpdaid_params(&buffer);
	
		addOptions(&buffer,
				true /* writer? */, q->segindex /* segindex */,true /* primary? */ , superuser());
	
	
		if (!cdbconn_doDispatchAgentConnect(&dispatchagents->db_descriptors[i], buffer.data))
		{      
	    	elog(ERROR,"createDispatchAgents failed %s",dispatchagents->db_descriptors[i].error_message.data);
	    }
	    /*
	     * Successfully connected.
	     */
	    else
	    {
	    	elog(DEBUG5,"createDispatchAgent %i success %s", i, buffer.data);
	    }
	}

    free(buffer.data);
    elog(DEBUG1, "createDispatchAgent completed successfully");
    
    largest_gangsize = dispatchagents->size;
    
    if (DEBUG1 >= log_min_messages)
    	{
    		char		msec_str[32];
    		switch (check_log_duration(msec_str, false))
    		{
    			case 1:
    			case 2:
    				ereport(LOG, (errmsg("duration to dispatch agents created: %s ms", msec_str)));
    				break;
    		}
    	}
	
	MemoryContextSwitchTo(oldContext);
	
	return dispatchagents;
}

/*
 * Every gang created must have a unique identifier, so the QD and Dispatch Agents can agree
 * about what they are talking about.
 * 
 * Since there can only be one primary writer gang, and only one mirror writer gang, we 
 * use id 1 and 2 for those two (helps in debugging).
 * 
 * Reader gang ids start at 3
 */

static int gang_id_counter = 3;
/*
 * creates a new gang by logging on a session to each segDB involved
 *
 */
static Gang *
createGang(GangType type, int gang_id, int size, int content, char *portal_name)
{
	Gang	   *newGangDefinition;
	MemoryContext oldContext;
	int			successful_connections = 0;

	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *q;
	DoConnectParms *doConnectParmsAr = NULL;
	int			threadCount = 0;
	int			max_threads;
	int			segdbs_in_thread_pool;
	int			segdb_count;
	int			i;

	Assert(size == 1 || size == getgpsegmentCount());

	/*
	 * We can't allocate more segments than we know about
	 */
	Assert(size <= getgpsegmentCount());
	
	Assert(CurrentResourceOwner != NULL);

	/*
	 * If we are going to use Dispatch Agents, we need to make sure they are running
	 */
	if (Gp_role == GP_ROLE_DISPATCH && gp_use_dispatch_agent && dispatchagents==NULL)
		createDispatchAgents();

	/*
	 * MemoryContexts used in this routine.
	 */
	if (GangContext == NULL)
	{ 
		GangContext = AllocSetContextCreate(TopMemoryContext,
											 "Gang Context",
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
	}

	oldContext = MemoryContextSwitchTo(GangContext);

	newGangDefinition = buildGangDefinition(type, gang_id, size, content, portal_name);

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
	elog(DEBUG1,"createGang type = %d, gang_id %d, size %d",type,gang_id,size);
	if (type == GANGTYPE_PRIMARY_WRITER)
		Assert(gang_id == 1);

	segdb_count = newGangDefinition->size;

	Assert(gp_connections_per_thread >= 0);
	Assert(segdb_count > 0);
	
	/*
	 * If we are using Dispatch Agents, assign the segDBs to the agents, and ask them to set 
	 * up the gang.
	 * 
	 * NOTE:  Entry DB gangs do NOT use dispatch agents.
	 */
	if (Gp_role == GP_ROLE_DISPATCH && gp_use_dispatch_agent && dispatchagents && type != GANGTYPE_ENTRYDB_READER)
	{
		PGresult   *result;
		int i, j;
		int whichagent = 0;
		GpSegmentConnectionResponse *gresp;
		GpSegmentConnectionInfoSet * conninfo;
		GpSegmentConnectionInfoSet ** conninfoarray = palloc0(sizeof(void*)*dispatchagents->size);
		
		
		
		Assert(Gp_role == GP_ROLE_DISPATCH);
		
		/*
		 * make one conninfo structure per dispatch agent
		 */
		for (whichagent=0;whichagent<dispatchagents->size;whichagent++)
		{
			// size should be reduced by number of dispatch agents
			int		conninfosize = sizeof(GpSegmentConnectionInfoSet) + size*sizeof(GpSegmentConnectionInfo);
			conninfo = palloc0(conninfosize);
			conninfo->nsegs = 0;				/* how many QEs this dispatch agent will control */
			conninfo->type = type;
			conninfo->gang_id = gang_id;
			conninfo->content = content;
			conninfoarray[whichagent] = conninfo;

		}
		
		
		for (j=0;j<newGangDefinition->size;j++)
		{
			/*
			 * We need to assign each QE that will be in the gang to a Dispatch Agent.
			 * 
			 * For now, this is an easy, but sub-optimal way:  Just round robin them to the agents.
			 * 
			 * There are several important optimizations missing:  First, any QEs on the same host
			 * as a Dispatch Agent should be controlled by that agent.  But, we don't have any info
			 * about which QEs are on which hosts.
			 * 
			 * Second, any QE that is from the same Postmaster as a Dispatch Agent should be 
			 * controlled by that dispatch agent.  This is especially useful when we are dealing
			 * with 1-gangs.
			 * 
			 */
			whichagent = j % dispatchagents->size;
			conninfo = conninfoarray[whichagent];
			Assert(conninfo);
			i = conninfo->nsegs;
			conninfo->seginfo[i].segindex = newGangDefinition->db_descriptors[j].segindex;
			conninfo->seginfo[i].isprimary = newGangDefinition->db_descriptors[j].segment_database_info->isprimary;
			strncpy(conninfo->seginfo[i].hostname, newGangDefinition->db_descriptors[j].segment_database_info->hostname,NAMEDATALEN);
			conninfo->seginfo[i].port = newGangDefinition->db_descriptors[j].segment_database_info->port;
			conninfo->nsegs ++; 
		}
		
		
		newGangDefinition->active = true;
		
		/*
		 * Now we must send a "create gang" message to the dispatch agents
		 */

		for (whichagent=0;whichagent<dispatchagents->size;whichagent++)
		{
			int i;
			conninfo = conninfoarray[whichagent];
			
			/*
			 * If we are creating a 1-gang, only 1 of the conninfo structs will
			 * have anything in it.  Skip the ones that don't.
			 */
			if (conninfo->nsegs == 0)
				continue;
			
			PQsendCreateGang(dispatchagents->db_descriptors[whichagent].conn, sizeof(GpSegmentConnectionInfoSet) + conninfo->nsegs*sizeof(GpSegmentConnectionInfo), conninfo );
			
			result = PQgetResult(dispatchagents->db_descriptors[whichagent].conn);
			
			if (PQresultStatus(result)!=PGRES_COMMAND_OK)
			{
				elog(ERROR,"Dispatch Agent unable to create gang, %s %s",PQresStatus(PQresultStatus(result)),
						PQresultErrorMessage(result));
			}
			
			 
			gresp = result->extras;
			
			/*
			 * this loop is just for debugging
			 */
			for (i = 0; i< conninfo->nsegs; i++)
			{
				elog(LOG,"DA %d: Gang_id %d Result %d MotionListener %d/%d  pid %d",whichagent, gang_id,i, 
						(gresp[i].motionListener & 0x0ffff),
						((gresp[i].motionListener>>16) & 0x0ffff), gresp[i].pid); 
			}
			
			/*
			 * Get the response from the agent, and fill in the gang with the motionListener and backendPid
			 * information.
			 */
	
			i = 0;	
			for (j=0;j<newGangDefinition->size;j++)
			{
				/*
				 * This IF statement relys on how the QEs where assigned to the agents above.
				 * If that method changes, this needs to change too.
				 */
				if (whichagent == j % dispatchagents->size)
				{
					conninfo = conninfoarray[whichagent];
					Assert(i < conninfo->nsegs);
					elog(LOG,"DA %d: Gang_id %d segdb %d",whichagent, gang_id, newGangDefinition->db_descriptors[j].segindex); 

					newGangDefinition->db_descriptors[j].motionListener = gresp[i].motionListener;
					newGangDefinition->db_descriptors[j].backendPid = gresp[i].pid;
					newGangDefinition->db_descriptors[j].myAgent = &dispatchagents->db_descriptors[whichagent];
					i++;
				}
				 
			}
			
			free(result->extras);		/* malloc'd, so use free */
			result->extras = NULL;
			
			PQclear(result);
			
			/*
			 * We need to keep track of which dispatch agent(s) control this gang, so we 
			 * know who to send gang control messages to.
			 * 
			 * For one gangs, we build a 1 list of dispatch agents.  This leaks these 2 palloc calls,
			 * but and given session only uses a handful of 1-gangs during it's life, so
			 * it's not worth worrying about it.
			 */
			if (newGangDefinition->size == 1)
			{				
				newGangDefinition->dispatchagents = palloc0(sizeof(DispatchAgentSet));
				newGangDefinition->dispatchagents->size = 1;
				newGangDefinition->dispatchagents->db_descriptors = (SegmentDatabaseDescriptor *) palloc0( sizeof(SegmentDatabaseDescriptor));
				newGangDefinition->dispatchagents->db_descriptors[0] = dispatchagents->db_descriptors[whichagent];
			}
		}
		
		/*
		 * By filling in the Gang->dispatchagents, we let dispatch know that it is supposed
		 * to use the dispatch agents (and what those dispatch agents are).
		 * 
		 * For n-gangs, we just point to the list of all dispatch agents.
		 * 
		 * If we ever support gangs that are > 1, but < number of segments, we'd need to update this.
		 * 
		 */
		if (newGangDefinition->dispatchagents==NULL || newGangDefinition->size > 1)
			newGangDefinition->dispatchagents = dispatchagents;
			
		newGangDefinition->all_valid_segdbs_connected = true;
		newGangDefinition->active = false;
		
		if (DEBUG1 >= log_min_messages)
			{
				char		msec_str[32];
				switch (check_log_duration(msec_str, false))
				{
					case 1:
					case 2:
						ereport(LOG, (errmsg("duration to gang_id %d created: %s ms", gang_id,msec_str)));
						break;
				}
			}
		
		MemoryContextSwitchTo(oldContext);
		
		return newGangDefinition;
	}
	
	/*
	 * If we get here, this gang creation isn't going to use Dispatch Agents
	 */

	/*
	 * The most threads we could have is segdb_count / gp_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / gp_connections_per_thread.
	 * We allocate enough memory for this many DoConnectParms structures,
	 * even though we may not use them all.
	 */
	
	if (size > largest_gangsize)
		largest_gangsize = size;
	
	if (gp_connections_per_thread==0)
		max_threads = 1;
	else
		max_threads = 1 + (segdb_count - 1) / gp_connections_per_thread;
	Assert(max_threads > 0);
	doConnectParmsAr = (DoConnectParms *)
		palloc0(max_threads * sizeof(DoConnectParms));
	segdbs_in_thread_pool = 0;

	newGangDefinition->active = true;


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
		 * This function handles putting the segdbDesc into the proper thread's Parms
		 * structure, based on gp_connections_per_thread.
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
	else if (gp_connections_per_thread == 0)
		threadCount = 1;
	else
		threadCount = 1 + (segdbs_in_thread_pool - 1) / gp_connections_per_thread;

	for (i = 0; i < threadCount; i++)
	{
		int pthread_err;
		DoConnectParms *pParms = &doConnectParmsAr[i];

		/* attributes of the thread we're creating */
		pthread_attr_t t_atts;

		/* set the gangType. */
		pParms->type = type;
		
		pParms->i_am_superuser = superuser();

        if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
		    elog(DEBUG5, "createGang creating thread %d of %d for libpq connections",
			     i + 1, threadCount);

		/* save ourselves some memory: the defaults for thread stack
		 * size are large (1M+) */
		pthread_attr_init(&t_atts);
#ifdef pg_on_solaris
		/* Solaris doesn't have PTHREAD_STACK_MIN ? */
		pthread_attr_setstacksize(&t_atts, (64*1024));
#else
		pthread_attr_setstacksize(&t_atts, Max(PTHREAD_STACK_MIN, (64*1024)));
#endif

		pthread_err = pthread_create(&pParms->thread, &t_atts, thread_DoConnect, pParms);

		pthread_attr_destroy(&t_atts);
		if (pthread_err != 0)
		{
			int j;

			/* Error during thread create (this should be caused by
			 * resource constraints). If we leave the threads running,
			 * they'll immediately have some problems -- so we need to
			 * join them, and *then* we can issue our FATAL error */

			for (j = 0; j < threadCount + (i - 1); j++)
			{
				DoConnectParms *pParms;

				pParms = &doConnectParmsAr[j];
				pthread_join(pParms->thread, NULL);
			}

			ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("CreateGang: failed to create thread %d of %d", i + 1, threadCount),
							errdetail("pthread_create() failed with err %d gp_connections_per_thread %d", pthread_err, gp_connections_per_thread),
							errhint("gp_connections_per_thread controls the number of threads created during parallel dispatch, current setting is %d increase the setting to spawn fewer threads", gp_connections_per_thread)));
		}

	}

	/*
	 * wait for all of the DoConnect threads to complete.
	 */
	for (i = 0; i < threadCount; i++)
	{
		DoConnectParms *pParms = &doConnectParmsAr[i];

        if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
    		elog(DEBUG5, "createGang joining to thread %d of %d for libpq connections",
			     i + 1, threadCount);

		if (0 != pthread_join(pParms->thread, NULL))
			elog(FATAL, "createGang: pthread_join failed on thread %d of %d",
				 i + 1, threadCount);
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

	newGangDefinition->active = false;

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
		if (!segdbDesc->conn)
		{
			/*
			 * Log failed connections.	Complete failures
			 * are taken care of later.
			 */
			ereport(LOG, (errcode(segdbDesc->errcode),
					  errmsg("Warning: %s", segdbDesc->error_message.data)));
			ereport(WARNING, (errcode(ERRCODE_WARNING_GP_INTERCONNECTION),
							  errmsg("%s", segdbDesc->error_message.data)));
			if (segdbDesc->errcode == 0 && segdbDesc->error_message.len == 0)
				elog(ERROR, "Connection is null, but no error code or error message, for segDB %d", i);
			Assert(segdbDesc->errcode != 0 &&
				   segdbDesc->error_message.len > 0);
			segdbDesc->errcode = 0;
			resetPQExpBuffer(&segdbDesc->error_message);

			newGangDefinition->all_valid_segdbs_connected = false;
		}
		else
		{
			Assert(PQstatus(segdbDesc->conn) != CONNECTION_BAD &&
				   segdbDesc->errcode == 0 &&
				   segdbDesc->error_message.len == 0);

            if (gp_log_gang >= GPVARS_VERBOSITY_VERBOSE)
                elog(LOG, "Connected to %s motionListenerPorts=%d/%d with options %s",
				     segdbDesc->whoami,
					 (segdbDesc->motionListener & 0x0ffff),
					 ((segdbDesc->motionListener>>16) & 0x0ffff),
				     PQoptions(segdbDesc->conn));

			/*
			 * We should have retrieved the IP from our cache
			 */
			Assert(q->hostip != NULL);

			/*
			 * We have a live connection!
			 */
			successful_connections++;
		}

	}

    if (size != successful_connections ||
        size != newGangDefinition->size)
    {
        elog(LOG, "createGang: %d processes requested; %d segments online; "
             "%d successful connections", size, newGangDefinition->size,
             successful_connections);
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

			/* clean the gang before error out */
			disconnectAndDestroyGang(newGangDefinition);

			ereport(ERROR, (errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
							errmsg("All segment databases are unavailable")));
		}
	}
	 

	MemoryContextSwitchTo(oldContext);

	if (successful_connections != newGangDefinition->size && isFTSEnabled())
	{
		bool		errorOut;

		if (type == GANGTYPE_MIRROR_WRITER && successful_connections == 0)
			mirroringNotConfigured = true;

		errorOut = FtsHandleGangConnectionFailure(newGangDefinition->db_descriptors,
												  newGangDefinition->size);

		disconnectAndDestroyGang(newGangDefinition);

		elog((errorOut ? ERROR : WARNING), "Greenplum Database detected segment failure(s), system is reconnected in %s mode", isFtsReadOnlySet() ? "read-only" : "read-write");

		newGangDefinition = NULL;
	}
	return newGangDefinition;
}


/*
 *	buildGangDefinitionFromConninfo: uses information sent to dispatch agent by the QD and
 *	it then converts this to a Gang structure and initializes
 *	all the non-connection related fields.
 * 
 *  Because I was in a hurry, and wanted to re-use the maximum amount of code, we use the
 *  normal Gang structure that we have always used.  I think we should eventually clean this
 *  up and make it a bit better.
 */
static Gang *
buildGangDefinitionFromConninfo(GpSegmentConnectionInfoSet * conninfo)
{
	Gang	   *newGangDefinition = NULL;

	SegmentDatabaseDescriptor *segdbDesc;

	int			i;
	int			size = conninfo->nsegs;
	GangType	type = conninfo->type;

    Assert(CurrentMemoryContext == GangContext);
    Assert(size > 0);

    /*
	 * Get the structure representing the configured segment
	 * databases.  This structure is fetched into a permanent memory
	 * context. We also allocate
	 * space for and partially construct the data structure pointed to
	 * by ClusterDefn and headed by CdbClusterDescriptor.
	 */
	newGangDefinition = (Gang *) palloc0(sizeof(Gang));

	newGangDefinition->type = conninfo->type;
	newGangDefinition->size = conninfo->nsegs;
	newGangDefinition->segdb_count = conninfo->nsegs;
	newGangDefinition->gang_id = conninfo->gang_id;
	newGangDefinition->allocated = false;
	newGangDefinition->active = false;
	newGangDefinition->portal_name = NULL;

    if (gp_log_gang >= GPVARS_VERBOSITY_VERBOSE)
    {
        const char *pn1 = "";
        const char *pn2 = "";
        const char *pn3 = "";

         
        switch (type)
	    {
		    case GANGTYPE_PRIMARY_WRITER:
			    elog(LOG, "buildGangDefinitionFromConninfo Starting %d qExec processes for primary writer gang%s%s%s",
			    		conninfo->nsegs, pn1, pn2, pn3);
			    break;
		    case GANGTYPE_MIRROR_WRITER:
			    elog(LOG, "buildGangDefinitionFromConninfo Starting %d qExec processes for mirror writer gang%s%s%s",
			    		conninfo->nsegs, pn1, pn2, pn3);
			    break;
		    case GANGTYPE_ENTRYDB_READER:
			    elog(LOG, "buildGangDefinitionFromConninfo Starting 1 qExec process for entry db reader gang%s%s%s",
                     pn1, pn2, pn3);
                break;
		    case GANGTYPE_PRIMARY_READER:
			    /* Must be size 1 or size numsegments */
                if (size == 1)
			        elog(LOG, "buildGangDefinitionFromConninfo Starting 1 qExec process for seg%d reader gang%s%s%s",
			        		conninfo->content, pn1, pn2, pn3);
                else
			        elog(LOG, "buildGangDefinitionFromConninfo Starting %d qExec processes for reader gang%s%s%s",
			        		conninfo->nsegs, pn1, pn2, pn3);
			    break;
		    case GANGTYPE_UNALLOCATED:
		    default:
			    Insist(0);
	    }
    }

	newGangDefinition->all_valid_segdbs_connected = false;



	Assert(newGangDefinition->segdb_count > 0);


	newGangDefinition->db_descriptors =
		(SegmentDatabaseDescriptor *) palloc0(conninfo->nsegs *
											  sizeof
											  (SegmentDatabaseDescriptor));


	for (i = 0; i<conninfo->nsegs; i++)
	{
		
		CdbComponentDatabaseInfo  *cdbinfo = palloc0(sizeof(CdbComponentDatabaseInfo));
		segdbDesc = &newGangDefinition->db_descriptors[i];
		memset(segdbDesc, 0, sizeof(*segdbDesc));

		cdbinfo->segindex = conninfo->seginfo[i].segindex;
		cdbinfo->isprimary = conninfo->seginfo[i].isprimary;
		cdbinfo->valid = true;
		cdbinfo->definedprimary = conninfo->seginfo[i].isprimary;
		cdbinfo->hostname = pstrdup(conninfo->seginfo[i].hostname);
		cdbinfo->hostip = NULL;
		cdbinfo->port = conninfo->seginfo[i].port;
		{
			int			ret;
			char		portNumberStr[32];
			char	   *service;
			struct addrinfo *addrs = NULL,
					   *addr;
			struct addrinfo hint;
			
			/* Initialize hint structure */
			MemSet(&hint, 0, sizeof(hint));
			hint.ai_socktype = SOCK_STREAM;
			hint.ai_family = AF_UNSPEC;
			
			snprintf(portNumberStr, sizeof(portNumberStr), "%d", conninfo->seginfo[i].port);
			service = portNumberStr;
				
			ret = pg_getaddrinfo_all(conninfo->seginfo[i].hostname, service, &hint, &addrs);
			if (ret || !addrs)
			{
				if (addrs)
					pg_freeaddrinfo_all(hint.ai_family, addrs);
				ereport(FATAL,
						(errmsg("could not translate host name \"%s\", port \"%d\" to address: %s",
								conninfo->seginfo[i].hostname, conninfo->seginfo[i].port, gai_strerror(ret))));
			}
			for (addr = addrs; addr; addr = addr->ai_next)
			{
#ifdef HAVE_UNIX_SOCKETS
				/* Ignore AF_UNIX sockets, if any are returned. */
				if (addr->ai_family == AF_UNIX)
					continue;
#endif
				if (addr->ai_family == AF_INET) /* IPv4 address */
				{
					char		hostinfo[NI_MAXHOST];
					pg_getnameinfo_all((struct sockaddr_storage *)addr->ai_addr, addr->ai_addrlen,
							hostinfo, sizeof(hostinfo),
												   NULL, 0,
												   NI_NUMERICHOST);
					cdbinfo->hostip = pstrdup(hostinfo);
					break;
				}
			}
			
#ifdef HAVE_IPV6
			if (cdbinfo->hostip == NULL && addrs->ai_family == AF_INET6)
				elog(ERROR,"host %s only has IPv6 address, not yet supported",conninfo->seginfo[i].hostname);
#endif
			
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		 
		}
		 
		
	    /* Segment db info */
	    segdbDesc->segment_database_info = cdbinfo;
	    segdbDesc->segindex = conninfo->seginfo[i].segindex;
	    
	    /* Connection info */
	    segdbDesc->conn = NULL;
	    segdbDesc->motionListener = 0;
	    segdbDesc->whoami = NULL;

	    /* Connection error info */
	    segdbDesc->errcode = 0;
	    initPQExpBuffer(&segdbDesc->error_message);
		
	}
	
	return newGangDefinition;
}



/*
 *	buildGangDefinition: reads the GP catalog tables and build a CdbComponentDatabases structure.
 *	It then converts this to a Gang structure and initializes
 *	all the non-connection related fields.
 */
static Gang *
buildGangDefinition(GangType type, int gang_id, int size, int content, char *portal_name)
{
	Gang	   *newGangDefinition = NULL;

	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *p;

	int			seg_count = 0;
	int			last_segindex = -1;
	int			i;

    Assert(CurrentMemoryContext == GangContext);
    Assert(size > 0);

    /*
	 * Get the structure representing the configured segment
	 * databases.  This structure is fetched into a permanent memory
	 * context. We also allocate
	 * space for and partially construct the data structure pointed to
	 * by ClusterDefn and headed by CdbClusterDescriptor.
	 */
	newGangDefinition = (Gang *) palloc0(sizeof(Gang));

	newGangDefinition->type = type;
	newGangDefinition->size = size;
	newGangDefinition->gang_id = gang_id;
	newGangDefinition->allocated = false;
	newGangDefinition->active = false;
	newGangDefinition->portal_name = (portal_name ? pstrdup(portal_name) : (char *) NULL);

    if (gp_log_gang >= GPVARS_VERBOSITY_VERBOSE)
    {
        const char *pn1 = "";
        const char *pn2 = "";
        const char *pn3 = "";

        if (portal_name)
        {
            pn1 = "; portal='";
            pn2 = portal_name;
            pn3 = "'";
        }

        switch (type)
	    {
		    case GANGTYPE_PRIMARY_WRITER:
			    elog(LOG, "Starting %d qExec processes for primary writer gang%s%s%s",
                     size, pn1, pn2, pn3);
			    break;
		    case GANGTYPE_MIRROR_WRITER:
			    elog(LOG, "Starting %d qExec processes for mirror writer gang%s%s%s",
                     size, pn1, pn2, pn3);
			    break;
		    case GANGTYPE_ENTRYDB_READER:
			    elog(LOG, "Starting 1 qExec process for entry db reader gang%s%s%s",
                     pn1, pn2, pn3);
                break;
		    case GANGTYPE_PRIMARY_READER:
			    /* Must be size 1 or size numsegments */
                if (size == 1)
			        elog(LOG, "Starting 1 qExec process for seg%d reader gang%s%s%s",
                         content, pn1, pn2, pn3);
                else
			        elog(LOG, "Starting %d qExec processes for reader gang%s%s%s",
                         size, pn1, pn2, pn3);
			    break;
		    case GANGTYPE_UNALLOCATED:
		    default:
			    Insist(0);
	    }
    }

	newGangDefinition->all_valid_segdbs_connected = false;

	/* retry transient failed dbs */
	if (type == GANGTYPE_PRIMARY_WRITER)
		FtsEvaluateTransientState();

	cdb_component_dbs = getCdbComponentDatabases();

	if (cdb_component_dbs == NULL || cdb_component_dbs->total_segments <= 0
		|| cdb_component_dbs->total_segment_dbs <= 0)
	{
		elog(ERROR, "building Gang:  GP schema not populated");
		return NULL;
	}

    /* 1-gang on entry db? */
    if (type == GANGTYPE_ENTRYDB_READER)
    {
    	CdbComponentDatabaseInfo  *cdbinfo = &cdb_component_dbs->entry_db_info[0];
    	
		if (cdbinfo->hostip == NULL)
		{
			/* we know we want to use localhost */
			cdbinfo->hostip = "127.0.0.1";
		}
		
        Assert(size == 1 && content == -1);
		
        /* Build segdb descriptor. */
        segdbDesc = (SegmentDatabaseDescriptor *)palloc0(sizeof(*segdbDesc));
		cdbconn_initSegmentDescriptor(segdbDesc,
									  cdbinfo);
	    newGangDefinition->size = 1;
	    newGangDefinition->segdb_count = 1;
	    newGangDefinition->db_descriptors = segdbDesc;

	    return newGangDefinition;
    }

	/* if mirroring is not configured */
	if (cdb_component_dbs->total_segment_dbs == cdb_component_dbs->total_segments)
	{
		elog(DEBUG2, "building Gang: mirroring not configured");
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
					elog(FATAL, "building Gang: Primary and mirror both unavailable for segment %d", p->segindex);
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
				else if (type == GANGTYPE_PRIMARY_READER && size == 1 &&
						 content != p->segindex)
				{
					last_segindex = p->segindex;
					continue;
				}
				else if (type == GANGTYPE_PRIMARY_WRITER && !p->isprimary)
				{
					/*
					 * this should not happen, or the pgdatabase info is wrong.
					 */
					elog(LOG, "Warning, potential wrong Greenplum Database configuration.");
					CondSetMPPReadonly();
				}
				else
				{
					segdbDesc = &newGangDefinition->db_descriptors[seg_count];

					cdbconn_initSegmentDescriptor(segdbDesc,
												  p);

					seg_count++;
					if (type == GANGTYPE_PRIMARY_READER && size == 1)
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
		elog(FATAL, "Not all primary segment instances are active and connected");
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

	
	Assert(newGangDefinition->size = seg_count || type == GANGTYPE_MIRROR_WRITER);
	newGangDefinition->size = seg_count;
	return newGangDefinition;
}


/*
 * AddSegDBToConnThreadPool
 * Helper function used to add a segdb's segdbDesc to the thread pool to be connected to.
 * It figures out which thread will handle it, based on the setting of
 * gp_connections_per_thread.
 */
void
addSegDBToConnThreadPool(DoConnectParms * ParmsAr, int *segdbCount,
						 SegmentDatabaseDescriptor * segdbDesc)
{
	DoConnectParms *pParms;
	int			ParmsIndex;
	bool		firsttime = false;

	Assert(segdbCount != NULL);

	/*
	 * The proper index into the DoConnectParms array is computed, based on
	 * having gp_connections_per_thread segdbDesc's in each thread.
	 * If it's the first access to an array location, determined
	 * by (*segdbCount) % gp_connections_per_thread == 0,
	 * then we initialize the struct members for that array location first.
	 */
	if (gp_connections_per_thread == 0)
		ParmsIndex = 0;
	else
		ParmsIndex = (*segdbCount) / gp_connections_per_thread;
	pParms = &ParmsAr[ParmsIndex];

	if (gp_connections_per_thread == 0)
		firsttime = (*segdbCount) == 0;
	else
		firsttime = (*segdbCount) % gp_connections_per_thread == 0;
	if (firsttime)
	{
		Assert(gp_connections_per_thread >= 0);
		pParms->segdbDescPtrArray =
			(SegmentDatabaseDescriptor **) palloc0((gp_connections_per_thread == 0 ? getgpsegmentCount() : gp_connections_per_thread) *
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

static void addOptions(PQExpBufferData * buffer, bool iswriter, int segindex, bool isprimary, bool i_am_superuser)
{
	int		j, start, end;
	char		temp[256];
	
	
	
	appendPQExpBufferStr(buffer, "options='");

	if (iswriter)
		appendPQExpBufferStr(buffer, "-c gp_is_writer=true ");

	appendPQExpBuffer(buffer,
					  "-c gp_segment=%d -c gp_max_packet_size=%d",
					  segindex, Gp_max_packet_size);

	if (!isprimary)
		appendPQExpBuffer(buffer, " -c gp_is_primary=false");
	
	appendPQExpBuffer(buffer, " -c gp_enable_functions=%d",gp_enable_functions);
	appendPQExpBuffer(buffer, " -c gp_qd_callback_info=port=%d",PostPortNumber);
	
	appendPQExpBuffer(buffer, " -c gp_is_callback=%d",gp_is_callback);
	appendPQExpBuffer(buffer, " -c gp_qd_proc_offset=%ld",(long)MAKE_OFFSET(MyProc));		
	
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CdbComponentDatabaseInfo  *qdinfo;
		
		qdinfo = &cdb_component_dbs->entry_db_info[0];
		if (qdinfo->hostip != NULL)
			appendPQExpBuffer(buffer, " -c gp_qd_hostname=%s", qdinfo->hostip);
		else
			appendPQExpBuffer(buffer, " -c gp_qd_hostname=%s", qdinfo->hostname);		
		appendPQExpBuffer(buffer, " -c gp_qd_port=%d", qdinfo->port);
	}
	else
	{
		appendPQExpBuffer(buffer, " -c gp_qd_hostname=%s", qdHostname);
		appendPQExpBuffer(buffer, " -c gp_qd_port=%d", qdPostmasterPort);
	}
	
	
	/*
     * user-settable GUC variables
     */

#ifdef USE_ASSERT_CHECKING
	appendPQExpBuffer(buffer, " -c gp_debug_linger=%d", gp_debug_linger);
#endif

	/*
	 * It doesn't seem to work to quote the entire search path,
	 * so we need to remove any spaces so it's treated as one parameter.
	 * and remove any quotes surrounding the whole thing.
	 *
	 */
	temp[0] = '\0';
	j = 0;
	start = 0;				/* start */
	end = strlen(namespace_search_path);	/* end */
	if ((namespace_search_path[0] == '\"' && namespace_search_path[strlen(namespace_search_path) - 1] == '\"') ||
		(namespace_search_path[0] == '\'' && namespace_search_path[strlen(namespace_search_path) - 1] == '\''))
	{
		start = 1;
		end = strlen(namespace_search_path) - 1;
	}
	for (; start < end; start++)
	{
		if (namespace_search_path[start] != ' ')
			temp[j++] = namespace_search_path[start];
	}
	temp[j] = '\0';

	appendPQExpBuffer(buffer, " -c search_path=%s", temp);

	switch (client_min_messages)
	{
		case DEBUG5:
			appendPQExpBuffer(buffer, " -c client_min_messages=DEBUG5");
			break;
		case DEBUG4:
			appendPQExpBuffer(buffer, " -c client_min_messages=DEBUG4");
			break;
		case DEBUG3:
			appendPQExpBuffer(buffer, " -c client_min_messages=DEBUG3");
			break;
		case DEBUG2:
			appendPQExpBuffer(buffer, " -c client_min_messages=DEBUG2");
			break;
		case DEBUG1:
			appendPQExpBuffer(buffer, " -c client_min_messages=DEBUG1");
			break;
		case LOG:
			appendPQExpBuffer(buffer, " -c client_min_messages=LOG");
			break;
		case INFO:
			appendPQExpBuffer(buffer, " -c client_min_messages=INFO");
			break;
		case NOTICE:
			appendPQExpBuffer(buffer, " -c client_min_messages=NOTICE");
			break;
		case WARNING:
			appendPQExpBuffer(buffer, " -c client_min_messages=WARNING");
			break;
		case ERROR:
			appendPQExpBuffer(buffer, " -c client_min_messages=ERROR");
			break;
		case FATAL:
			appendPQExpBuffer(buffer, " -c client_min_messages=FATAL");
			break;
		case PANIC:
			appendPQExpBuffer(buffer, " -c client_min_messages=PANIC");
			break;
		default:;
	}

	appendPQExpBuffer(buffer, " -c work_mem=%d", work_mem);
	appendPQExpBuffer(buffer, " -c maintenance_work_mem=%d", maintenance_work_mem);
	appendPQExpBuffer(buffer, " -c temp_buffers=%d", num_temp_buffers);
    appendPQExpBuffer(buffer, " -c gp_log_interconnect=%s -c gp_interconnect_type=%s",
					  gpvars_show_gp_log_interconnect(), gpvars_show_gp_interconnect_type());

    if (gp_use_snapshop_during_callback)
    	appendPQExpBuffer(buffer, " -c gp_use_snapshop_during_callback=true");
    else
    	appendPQExpBuffer(buffer, " -c gp_use_snapshop_during_callback=false");

	if (StatementTimeout != 0)
		appendPQExpBuffer(buffer, " -c statement_timeout=%d", StatementTimeout);
	if (CommitDelay != 0)
		appendPQExpBuffer(buffer, " -c commit_delay=%d", CommitDelay);
	if (CommitSiblings != 5)
		appendPQExpBuffer(buffer, " -c commit_siblings=%d", CommitSiblings);
	if (interconnect_setup_timeout != 10)
		appendPQExpBuffer(buffer, " -c gp_interconnect_setup_timeout=%d", interconnect_setup_timeout);
	if (gp_hashagg_rewrite_limit != 1.0)
		appendPQExpBuffer(buffer, " -c gp_hashagg_rewrite_limit=%f", gp_hashagg_rewrite_limit);
	if (gp_hashagg_spillbatch_min != 0)
		appendPQExpBuffer(buffer, " -c gp_hashagg_spillbatch_min=%d", gp_hashagg_spillbatch_min);
	if (gp_hashagg_spillbatch_max != 0)
		appendPQExpBuffer(buffer, " -c gp_hashagg_spillbatch_max=%d", gp_hashagg_spillbatch_max);


	appendPQExpBuffer(buffer, " -c DateStyle=");
	switch (DateStyle)
	{
		case USE_ISO_DATES:
			appendPQExpBuffer(buffer, "ISO");
			break;
		case USE_SQL_DATES:
			appendPQExpBuffer(buffer, "SQL");
			break;
		case USE_GERMAN_DATES:
			appendPQExpBuffer(buffer, "German");
			break;
		default:
			appendPQExpBuffer(buffer, "Postgres");
			break;
	}
	switch (DateOrder)
	{
		case DATEORDER_YMD:
			appendPQExpBuffer(buffer, ",YMD");
			break;
		case DATEORDER_DMY:
			appendPQExpBuffer(buffer, ",DMY");
			break;
		default:
			appendPQExpBuffer(buffer, ",MDY");
			break;
	}

	if (strcmp(show_timezone(), "unknown") != 0)
	{
		appendPQExpBuffer(buffer, " -c TimeZone=%s", show_timezone());
	}

	if (DefaultXactIsoLevel != XACT_READ_COMMITTED)
	{
		if (DefaultXactIsoLevel == XACT_SERIALIZABLE)
			appendPQExpBuffer(buffer, " -c default_transaction_isolation=serializable");

	}
	if (XactIsoLevel != XACT_READ_COMMITTED)
	{
		if (XactIsoLevel == XACT_SERIALIZABLE)
			appendPQExpBuffer(buffer, " -c transaction_isolation=serializable");

	}


	/*
     * some super-user settable gucs
     */
	if (i_am_superuser)
	{
		switch (log_min_messages)
		{
			case DEBUG5:
				appendPQExpBuffer(buffer, " -c log_min_messages=DEBUG5");
				break;
			case DEBUG4:
				appendPQExpBuffer(buffer, " -c log_min_messages=DEBUG4");
				break;
			case DEBUG3:
				appendPQExpBuffer(buffer, " -c log_min_messages=DEBUG3");
				break;
			case DEBUG2:
				appendPQExpBuffer(buffer, " -c log_min_messages=DEBUG2");
				break;
			case DEBUG1:
				appendPQExpBuffer(buffer, " -c log_min_messages=DEBUG1");
				break;
			case LOG:
				appendPQExpBuffer(buffer, " -c log_min_messages=LOG");
				break;
			case INFO:
				appendPQExpBuffer(buffer, " -c log_min_messages=INFO");
				break;

			case WARNING:
				appendPQExpBuffer(buffer, " -c log_min_messages=WARNING");
				break;
			case ERROR:
				appendPQExpBuffer(buffer, " -c log_min_messages=ERROR");
				break;
			case FATAL:
				appendPQExpBuffer(buffer, " -c log_min_messages=FATAL");
				break;
			case PANIC:
				appendPQExpBuffer(buffer, " -c log_min_messages=PANIC");
				break;
			default:;
		}

		if (Log_error_verbosity != PGERROR_DEFAULT)
		{
			if (Log_error_verbosity == PGERROR_VERBOSE)
				appendPQExpBuffer(buffer, " -c log_error_verbosity=verbose");
			if (Log_error_verbosity == PGERROR_TERSE)
				appendPQExpBuffer(buffer, " -c log_error_verbosity=terse");
		}

		if (log_duration)
			appendPQExpBuffer(buffer, " -c log_duration=true");
		
		if (log_dispatch_stats)
			appendPQExpBuffer(buffer, " -c log_dispatch_stats=true");
		if (log_statement_stats)
			appendPQExpBuffer(buffer, " -c log_statement_stats=true");
		if (log_executor_stats)
			appendPQExpBuffer(buffer, " -c log_executor_stats=true");

		if (log_min_duration_statement >= 0)
			appendPQExpBuffer(buffer, " -c log_min_duration_statement=%d", log_min_duration_statement);

	    appendPQExpBuffer(buffer, " -c gp_connections_per_thread=%d",gp_connections_per_thread);


		switch (log_min_error_statement)
		{
			case DEBUG5:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=DEBUG5");
				break;
			case DEBUG4:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=DEBUG4");
				break;
			case DEBUG3:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=DEBUG3");
				break;
			case DEBUG2:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=DEBUG2");
				break;
			case DEBUG1:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=DEBUG1");
				break;
			case LOG:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=LOG");
				break;
			case INFO:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=INFO");
				break;
			case NOTICE:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=NOTICE");
				break;
			case WARNING:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=WARNING");
				break;
			case ERROR:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=ERROR");
				break;
			case FATAL:
				appendPQExpBuffer(buffer, " -c log_min_error_statement=FATAL");
				break;

			default:;
		}



		if (gp_set_read_only)
			appendPQExpBuffer(buffer, " -c gp_set_read_only=true");

    }                       /* some super-user settable gucs */

	appendPQExpBuffer(buffer, "' ");
}

/*
 *	thread_DoConnect is the thread proc used to perform the connection to one of the qExecs.
 */
static void *
thread_DoConnect(void *arg)
{
	DoConnectParms *pParms;
	SegmentDatabaseDescriptor **segdbDescPtrArray;

	int			db_count;
	int			i;

	SegmentDatabaseDescriptor *segdbDesc;
	CdbComponentDatabaseInfo *q;

	pParms = (DoConnectParms *) arg;
	segdbDescPtrArray = pParms->segdbDescPtrArray;
	db_count = pParms->db_count;

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

		build_gpqeid_params(&buffer);


		addOptions(&buffer,
				(pParms->type == GANGTYPE_PRIMARY_WRITER || pParms->type == GANGTYPE_MIRROR_WRITER),
				q->segindex,q->isprimary,pParms->i_am_superuser);

		if (cdbconn_doConnect(segdbDesc, buffer.data))
		{
			if (segdbDesc->motionListener == -1)
			{
				segdbDesc->errcode = ERRCODE_GP_INTERNAL_ERROR;
				appendPQExpBuffer(&segdbDesc->error_message,
						  "Internal error: No motion listener port for %s\n",
								  segdbDesc->whoami);
				PQfinish(segdbDesc->conn);
				segdbDesc->conn = NULL;
			}
		}

		free(buffer.data);
	}

	return (NULL);
}	/* thread_DoConnect */


/*
 * build_gpqeid_params
 *
 * Called from the qDisp process to create the "gpqeid" parameter string
 * to be passed to a qExec that is being started.  NB: Can be called in a
 * thread, so mustn't use palloc/elog/ereport/etc.
 */
static void
build_gpqeid_params(PQExpBuffer buf)
{
	appendPQExpBufferStr(buf, "gpqeid=");

	appendPQExpBuffer(buf, "%d;", gp_session_id);

#ifdef HAVE_INT64_TIMESTAMP
	appendPQExpBuffer(buf, INT64_FORMAT ";", PgStartTime);
#else
#ifndef _WIN32
	appendPQExpBuffer(buf, "%.14a;", PgStartTime);
#else
	appendPQExpBuffer(buf, "%g;", PgStartTime);
#endif 
#endif

	/* change last semicolon to space */
	Assert(buf->data[buf->len - 1] == ';');
	buf->data[buf->len - 1] = ' ';
}	/* build_gpqeid_params */


/*
 * build_gpdaid_params
 *
 * Called from the qDisp process to create the "gpdaid" parameter string
 * to be passed to a QDA that is being started.  NB: Can be called in a
 * thread, so mustn't use palloc/elog/ereport/etc.
 */
static void
build_gpdaid_params(PQExpBuffer buf)
{
	appendPQExpBufferStr(buf, "gpdaid=");

	appendPQExpBuffer(buf, "%d;", gp_session_id);

#ifdef HAVE_INT64_TIMESTAMP
	appendPQExpBuffer(buf, INT64_FORMAT ";", PgStartTime);
#else
#ifndef _WIN32
	appendPQExpBuffer(buf, "%.14a;", PgStartTime);
#else
	appendPQExpBuffer(buf, "%g;", PgStartTime);
#endif 
#endif

	/* change last semicolon to space */
	Assert(buf->data[buf->len - 1] == ';');
	buf->data[buf->len - 1] = ' ';
}	/* build_gpdaid_params */


/*
 * cdbgang_parse_gpqeid_params
 *
 * Called very early in backend initialization, to interpret the "gpqeid"
 * parameter value that a qExec receives from its qDisp.
 *
 * At this point, client authentication has not been done; the backend
 * command line options have not been processed; GUCs have the settings
 * inherited from the postmaster; etc; so don't try to do too much in here.
 */
static bool
gpqeid_next_param(char **cpp, char **npp)
{
	*cpp = *npp;
	if (!*cpp)
		return false;

	*npp = strchr(*npp, ';');
	if (*npp)
	{
		**npp = '\0';
		++*npp;
	}
	return true;
}

void
cdbgang_parse_gpqeid_params(struct Port *port __attribute__((unused)) , const char *gpqeid_value)
{
	char	   *gpqeid = pstrdup(gpqeid_value);
	char	   *cp;
	char	   *np = gpqeid;

	/* The presence of an gpqeid string means this backend is a qExec. */
	SetConfigOption("gp_session_role", "execute",
					PGC_POSTMASTER, PGC_S_OVERRIDE);

	/* gp_session_id */
	if (gpqeid_next_param(&cp, &np))
		SetConfigOption("gp_session_id", cp,
						PGC_POSTMASTER, PGC_S_OVERRIDE);

	/* PgStartTime */
	if (gpqeid_next_param(&cp, &np))
	{
#ifdef HAVE_INT64_TIMESTAMP
		if (!scanint8(cp, true, &PgStartTime))
			goto bad;
#else
		PgStartTime = strtod(cp, NULL);
#endif
	}

	/* Too few items, or too many? */
	if (!cp || np)
		goto bad;

	if (gp_session_id <= 0 ||
		PgStartTime <= 0)
		goto bad;

	pfree(gpqeid);
	return;

bad:
	elog(FATAL, "QE dispatched with invalid option: 'gpqeid=%s'", gpqeid_value);
}	/* cdbgang_parse_gpqeid_params */


void
cdbgang_parse_gpqdid_params(struct Port *port __attribute__((unused)) , const char *gpqdid_value)
{
	char	   *gpqdid = pstrdup(gpqdid_value);
	char	   *cp;
	char	   *np = gpqdid;

	/* The presence of an gpqdid string means this is a callback from QE to QD. */
	SetConfigOption("gp_is_callback", "true",
					PGC_POSTMASTER, PGC_S_OVERRIDE);

	/* gp_session_id */
	if (gpqeid_next_param(&cp, &np))
		SetConfigOption("gp_session_id", cp,
						PGC_POSTMASTER, PGC_S_OVERRIDE);

	/* PgStartTime */
	if (gpqeid_next_param(&cp, &np))
	{
#ifdef HAVE_INT64_TIMESTAMP
		if (!scanint8(cp, true, &PgStartTime))
			goto bad;
#else
		PgStartTime = strtod(cp, NULL);
#endif
	}
	Assert(gp_is_callback);

	/* Too few items, or too many? */
	if (!cp || np)
		goto bad;

	if (gp_session_id <= 0 ||
		PgStartTime <= 0)
		goto bad;

	pfree(gpqdid);
	return;

bad:
	elog(FATAL, "QD callback dispatched with invalid option: 'gpqdid=%s'", gpqdid_value);
}	/* cdbgang_parse_gpqdid_params */

void
cdbgang_parse_gpdaid_params(struct Port *port __attribute__((unused)) , const char *gpdaid_value)
{
	char	   *gpdaid = pstrdup(gpdaid_value);
	char	   *cp;
	char	   *np = gpdaid;
	
	/* The presence of an gpdaid string means this backend is a Query Dispatch Agent (QDA). */
	SetConfigOption("gp_session_role", "dispatchagent",
					PGC_POSTMASTER, PGC_S_OVERRIDE);

	/* gp_session_id */
	if (gpqeid_next_param(&cp, &np))
		SetConfigOption("gp_session_id", cp,
						PGC_POSTMASTER, PGC_S_OVERRIDE);

	/* PgStartTime */
	if (gpqeid_next_param(&cp, &np))
	{
#ifdef HAVE_INT64_TIMESTAMP
		if (!scanint8(cp, true, &PgStartTime))
			goto bad;
#else
		PgStartTime = strtod(cp, NULL);
#endif
	}

	/* Too few items, or too many? */
	if (!cp || np)
		goto bad;

	if (gp_session_id <= 0 ||
		PgStartTime <= 0)
		goto bad;

	pfree(gpdaid);

	elog(DEBUG1," Dispatch Agent Started");
	return;

bad:
	elog(FATAL, "Query Dispatch Agent dispatched with invalid option: 'gpdaid=%s'", gpdaid_value);
}	/* cdbgang_parse_gpdaid_params */

/*
 * This is where we keep track of all the gangs that exist for this session.
 * On a QD, gangs can either be "available" (not currently in use), or "allocated".
 * 
 * On a Dispatch Agent, we just store them in the "available" lists, as the DA doesn't
 * keep track of allocations (it assumes the QD will keep track of what is allocated or not).
 *
 */

static List *allocatedReaderGangsN = NIL;
static List *availableReaderGangsN = NIL;
static List *allocatedReaderGangs1 = NIL;
static List *availableReaderGangs1 = NIL;
static Gang *primaryWriterGang = NULL;
static Gang *mirrorWriterGang = NULL;

List *
getAllReaderGangs()
{
	List	   *res = NIL;
	ListCell   *le;

	/*
	 * using list_concat() here will destructively modify the lists!
	 *
	 * res = list_concat(availableReaderGangsN,
	 * list_concat(availableReaderGangs1, list_concat(allocatedReaderGangsN,
	 * allocatedReaderGangs1)));
	 */
	foreach(le, availableReaderGangsN)
	{
		res = lappend(res, lfirst(le));
	}
	foreach(le, availableReaderGangs1)
	{
		res = lappend(res, lfirst(le));
	}
	foreach(le, allocatedReaderGangsN)
	{
		res = lappend(res, lfirst(le));
	}
	foreach(le, allocatedReaderGangs1)
	{
		res = lappend(res, lfirst(le));
	}

	return res;
}

/*
 * create a gang fragment via list of gang members
 */
GpSegmentConnectionResponse *
createGangFragment(GpSegmentConnectionInfoSet *conninfo, int * outsize)
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

	GpSegmentConnectionResponse *r;
 
	
	Assert(conninfo->nsegs > 0 && conninfo->nsegs < 10000);
	
	if (conninfo->nsegs > largest_gangsize)
		largest_gangsize = conninfo->nsegs;

	/*
	 * MemoryContexts used in this routine.
	 */
	if (GangContext == NULL)
	{
		GangContext = AllocSetContextCreate(TopMemoryContext,
											 "Gang Context",
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
	}
	Assert(GangContext != NULL);

	oldContext = MemoryContextSwitchTo(GangContext);

	elog(LOG,"Gang create fragment, id = %d, type = %d",conninfo->gang_id, conninfo->type);
	
	for (i = 0 ; i<conninfo->nsegs;i++)
    {
    	elog(LOG,"Gang connect segindex=%d, isprimary=%d, host=%s, port=%d",
    			conninfo->seginfo[i].segindex,
    			conninfo->seginfo[i].isprimary,
    			conninfo->seginfo[i].hostname,
    			conninfo->seginfo[i].port
    			);
    }
	
	if (Gp_role == GP_ROLE_DISPATCHAGENT)
		Assert(conninfo->type != GANGTYPE_ENTRYDB_READER);
	

	newGangDefinition = buildGangDefinitionFromConninfo(conninfo);
	
	/*
	 * The most threads we could have is segdb_count / gp_connections_per_thread, rounded up.
	 * This is equivalent to 1 + (segdb_count-1) / gp_connections_per_thread.
	 * We allocate enough memory for this many DoConnectParms structures,
	 * even though we may not use them all.
	 */
	segdb_count = conninfo->nsegs;
	if (gp_connections_per_thread == 0)
		max_threads = 1;
	else
		max_threads = 1 + (segdb_count - 1) / gp_connections_per_thread;
	Assert(max_threads > 0);
	doConnectParmsAr = (DoConnectParms *)
		palloc0(max_threads * sizeof(DoConnectParms));
	segdbs_in_thread_pool = 0;
	
 
	newGangDefinition->active = true;

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
		 * This function handles putting the segdbDesc into the proper thread's Parms
		 * structure, based on gp_connections_per_thread.
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
	else if (gp_connections_per_thread == 0)
		threadCount = 1;
	else
		threadCount = 1 + (segdbs_in_thread_pool - 1) / gp_connections_per_thread;

	for (i = 0; i < threadCount; i++)
	{
		int pthread_err;
		DoConnectParms *pParms = &doConnectParmsAr[i];

		/* attributes of the thread we're creating */
		pthread_attr_t t_atts;

		/* set the gangType. */
		pParms->type = conninfo->type;
		
		pParms->i_am_superuser = superuser();

        if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
		    elog(DEBUG5, "createGang creating thread %d of %d for libpq connections",
			     i + 1, threadCount);
        


		/* save ourselves some memory: the defaults for thread stack
		 * size are large (1M+) */
		pthread_attr_init(&t_atts);
#ifdef pg_on_solaris
		/* Solaris doesn't have PTHREAD_STACK_MIN ? */
		pthread_attr_setstacksize(&t_atts, (64*1024));
#else
		pthread_attr_setstacksize(&t_atts, Max(PTHREAD_STACK_MIN, (64*1024)));
#endif
		 
		pthread_err = pthread_create(&pParms->thread, NULL, thread_DoConnect, pParms);

		pthread_attr_destroy(&t_atts);
		if (pthread_err != 0)
		{
			int j;

			/* Error during thread create (this should be caused by
			 * resource constraints). If we leave the threads running,
			 * they'll immediately have some problems -- so we need to
			 * join them, and *then* we can issue our FATAL error */

			for (j = 0; j < threadCount + (i - 1); j++)
			{
				DoConnectParms *pParms;

				pParms = &doConnectParmsAr[j];
				pthread_join(pParms->thread, NULL);
			}

			ereport(FATAL, (errcode(ERRCODE_INTERNAL_ERROR),
							errmsg("CreateGang: failed to create thread %d of %d", i + 1, threadCount),
							errdetail("pthread_create() failed with err %d gp_connections_per_thread %d", pthread_err, gp_connections_per_thread),
							errhint("gp_connections_per_thread controls the number of threads created during parallel dispatch, current setting is %d increase the setting to spawn fewer threads", gp_connections_per_thread)));
		}

	}

	/*
	 * wait for all of the DoConnect threads to complete.
	 */
	for (i = 0; i < threadCount; i++)
	{
		DoConnectParms *pParms = &doConnectParmsAr[i];

        if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
    		elog(DEBUG5, "createGang joining to thread %d of %d for libpq connections",
			     i + 1, threadCount);

		if (0 != pthread_join(pParms->thread, NULL))
			elog(FATAL, "createGang: pthread_join failed on thread %d of %d",
				 i + 1, threadCount);
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

	newGangDefinition->active = false;

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
		if (!segdbDesc->conn)
		{
			/*
			 * Log failed connections.	Complete failures
			 * are taken care of later.
			 */
			ereport(ERROR, (errcode(ERRCODE_WARNING_GP_INTERCONNECTION),
							  errmsg("%s", segdbDesc->error_message.data)));
			if (segdbDesc->errcode == 0 && segdbDesc->error_message.len == 0)
				elog(ERROR, "Connection is null, but no error code or error message, for segDB %d", i);
			Assert(segdbDesc->errcode != 0 &&
				   segdbDesc->error_message.len > 0);
			segdbDesc->errcode = 0;
			resetPQExpBuffer(&segdbDesc->error_message);

			newGangDefinition->all_valid_segdbs_connected = false;
		}
		else
		{
			Assert(PQstatus(segdbDesc->conn) != CONNECTION_BAD &&
				   segdbDesc->errcode == 0 &&
				   segdbDesc->error_message.len == 0);

            if (gp_log_gang >= GPVARS_VERBOSITY_VERBOSE || DEBUG4 >= log_min_messages)
                elog(LOG, "Connected to %s motionListenerPorts=%d/%d with options %s",
				     segdbDesc->whoami,
					 (segdbDesc->motionListener & 0x0ffff),
					 ((segdbDesc->motionListener>>16) & 0x0ffff),
				     PQoptions(segdbDesc->conn));

			/*
			 * We should have retrieved the IP from our cache
			 */
			Assert(q->hostip != NULL);

			/*
			 * We have a live connection!
			 */
			successful_connections++;
		}

	}

    if (conninfo->nsegs != successful_connections ||
    		conninfo->nsegs != newGangDefinition->size)
    {
        elog(LOG, "createGang: %d processes requested; %d segments online; "
             "%d successful connections", conninfo->nsegs, newGangDefinition->size,
             successful_connections);
    }

	
	if (conninfo->type == GANGTYPE_PRIMARY_WRITER)
		primaryWriterGang = newGangDefinition;
	if (conninfo->type == GANGTYPE_MIRROR_WRITER)
		mirrorWriterGang = newGangDefinition;
		
	if (conninfo->type == GANGTYPE_PRIMARY_READER)
	{
		if (conninfo->nsegs==1)
			availableReaderGangs1 = lappend(availableReaderGangs1, newGangDefinition);
		else
			availableReaderGangsN = lappend(availableReaderGangsN, newGangDefinition);
	}
	


    MemoryContextSwitchTo(MessageContext);
	
	*outsize = sizeof(GpSegmentConnectionResponse)*conninfo->nsegs;
	r = palloc0(*outsize);
	
	for (i = 0 ; i<conninfo->nsegs;i++)
	{
		r[i].motionListener = newGangDefinition->db_descriptors[i].motionListener;
		r[i].pid = PQbackendPID(newGangDefinition->db_descriptors[i].conn);
		//strncpy(r[i].hostip, ?);	// Do we need this?
		elog(LOG,"Gang response motionListener %d/%d, pid %d",
				 (r[i].motionListener & 0x0ffff),
				 ((r[i].motionListener>>16) & 0x0ffff),
				 r[i].pid);
	}

	MemoryContextSwitchTo(oldContext);
	
	return r;
}

/*
 * This is the main routine.   It allocates a gang to a query.
 * If we have a previously created but unallocated gang lying around, we use that.
 * otherwise, we create a new gang
 * 
 * This is only used on the QD.
 *
 */
Gang *
allocateGang(GangType type, int size, int content, char *portal_name)
{
	/*
	 * First, we look for an unallocated but created gang of the right type
	 * if it exists, we return it.
	 * Else, we create a new gang
	 */

	MemoryContext oldContext;

	Gang	   *gp = NULL;

	Insist(Gp_role == GP_ROLE_DISPATCH);
	
    if (type == GANGTYPE_PRIMARY_WRITER)
    	return allocatePrimaryWriterGang();
    else if (type == GANGTYPE_MIRROR_WRITER)
    	return allocateMirrorWriterGang();

	if (!IsTransactionOrTransactionBlock())
	{
		elog(ERROR, "allocateGang must be called inside of transaction");
	}

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "allocateGang for portal %s: allocatedReaderGangsN %d, availableReaderGangsN %d",
		     (portal_name ? portal_name : "<unnamed>"),
		     list_length(allocatedReaderGangsN),
		     list_length(availableReaderGangsN));
    

    
    if (GangContext == NULL)
	{ 
		GangContext = AllocSetContextCreate(TopMemoryContext,
											 "Gang Context",
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
	}
	Assert(GangContext != NULL);

	oldContext = MemoryContextSwitchTo(GangContext);

	switch (type)
	{

        case GANGTYPE_PRIMARY_READER:
        case GANGTYPE_ENTRYDB_READER:
			/*
			 * Must be size 1 or size numsegments
			 * If size 1, then content tells us which segment we want
			 */
			if (size != 1)
			{
                Assert(type == GANGTYPE_PRIMARY_READER);
				if (availableReaderGangsN != NIL)		/* There are gangs
														 * already created */
				{
                    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
					    elog(DEBUG4, "Reusing an available reader N-gang for %s", (portal_name ? portal_name : "unnamed portal"));

					gp = linitial(availableReaderGangsN);
					Assert(gp != NULL);
					Assert(gp->type == type && gp->size > 1);
					
					/* make sure no memory is still allocated for previous portal name that this gang belonged to */
					if(gp->portal_name)
						pfree(gp->portal_name);
					
					/* let the gang know which portal it is being assigned to */
					gp->portal_name = (portal_name ? pstrdup(portal_name) : (char *) NULL);

					availableReaderGangsN =
						list_delete_first(availableReaderGangsN);

				}
				else
					/*
					 * no pre-created gang exists
					 */
				{
                    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
					    elog(DEBUG4, "Creating a new reader N-gang for %s", (portal_name ? portal_name : "unnamed portal"));
					gp = createGang(type, gang_id_counter++, size, content, portal_name);
					/* try again */
					if (gp == NULL)
						gp = createGang(type, gang_id_counter++, size, content, portal_name);
				}
				if (gp == NULL)
					elog(ERROR, "No reader gang allocated");

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
                    ListCell       *cell = list_head(availableReaderGangs1);
                    ListCell       *nextcell;
                    ListCell       *prevcell = NULL;

                    /* Look for a gang with the requested segment index. */
                    while (cell)
                    {
                        nextcell = lnext(cell);
                        gp = (Gang *)lfirst(cell);
					    Assert(gp->size == 1);
					    if (gp->db_descriptors[0].segindex == content)
                            break;
                        prevcell = cell;
                        cell = nextcell;
                    }

                    /* Remove gang from freelist and reuse. */
                    if (cell)
				    {
                        if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
					        elog(DEBUG4, "reusing an available reader 1-gang for seg%d, portal: %s", content, (portal_name ? portal_name : "unnamed"));

                        availableReaderGangs1 =
                            list_delete_cell(availableReaderGangs1, cell, prevcell);
						
					    /* make sure no memory is still allocated for previous portal name that this gang belonged to */
					    if(gp->portal_name)
						    pfree(gp->portal_name);
						
					    /* let the gang know which portal it is being assigned to */
					    gp->portal_name = (portal_name ? pstrdup(portal_name) : (char *) NULL);
				    }
					else
						gp = NULL;
				}
				if (gp == NULL) /* no pre-created gang exists */
				{
                    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
					    elog(DEBUG4, "creating a new reader 1-gang for seg%d, portal: %s", content, (portal_name ? portal_name : "unnamed"));
					gp = createGang(type, gang_id_counter++, size, content, portal_name);
					/* try again */
					if (gp == NULL || gp->size != size)
						gp = createGang(type, gang_id_counter++, size, content, portal_name);
				}
				if (gp == NULL || gp->size != size)
                {
                    if (content == -1)
					    elog(ERROR, "Unable to allocate a reader 1-gang for entry db; portal: %s", (portal_name ? portal_name : "unnamed"));
                    else
					    elog(ERROR, "Unable to allocate a reader 1-gang for seg%d; portal: %s", content, (portal_name ? portal_name : "unnamed"));
                }
				allocatedReaderGangs1 = lappend(allocatedReaderGangs1, gp);
			}
			break;

        case GANGTYPE_PRIMARY_WRITER:
        case GANGTYPE_MIRROR_WRITER:
        case GANGTYPE_UNALLOCATED:
		default:
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
							errmsg_internal("problem in allocateGang")));
			break;
	}

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
	    elog(DEBUG4,
		     "allocateGang on return: allocatedReaderGangsN %d, availableReaderGangsN %d",
		     list_length(allocatedReaderGangsN),
		     list_length(availableReaderGangsN));

	/*
	 * now we enroll the gang in the current global transaction?
	 */

	MemoryContextSwitchTo(oldContext);
	if (gp == NULL)
	{
		if (type != GANGTYPE_MIRROR_WRITER || !mirroringNotConfigured)
			elog(ERROR, "No %s gang allocated",
				 (type == GANGTYPE_PRIMARY_WRITER) ? "primary" : "mirror");
	}

	if (gp != NULL)
	{
		gp->allocated = true;

		/* sanity check the gang */
		if (!gangOK(gp))
			elog(ERROR, "Segment connection failed: allocateGang attempted to return a bad gang.");
	}

	return gp;
}


Gang *
allocatePrimaryWriterGang(void)
{
	/*
	 * First, we look for an unallocated but created gang of the right type
	 * if it exists, we return it.
	 * Else, we create a new gang
	 */

	MemoryContext oldContext;

	Gang	   *gp = NULL;

	Assert(Gp_role == GP_ROLE_DISPATCH);

	if (primaryWriterGang == NULL)
	{
		int		nsegdb;
		
		if (!IsTransactionOrTransactionBlock())
		{
			elog(ERROR, "allocatePrimaryWriterGang must be called inside of transaction");
		}
		
		if (GangContext == NULL)
		{ 
			GangContext = AllocSetContextCreate(TopMemoryContext,
												 "Gang Context",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);
		}
		Assert(GangContext != NULL);

		oldContext = MemoryContextSwitchTo(GangContext);
		
		nsegdb = getgpsegmentCount();
		/*
		 * try twice for creating gang
		 */
		gp = primaryWriterGang = createGang(GANGTYPE_PRIMARY_WRITER, 1, nsegdb, -1, NULL);
		if (gp == NULL)
			gp = primaryWriterGang = createGang(GANGTYPE_PRIMARY_WRITER, 1, nsegdb, -1, NULL);
		
		MemoryContextSwitchTo(oldContext);
	}
	else
	{
        if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
		    elog(DEBUG4, "Reusing an existing primary writer gang");
		gp = primaryWriterGang;
	}
	
	/*
	 * now we enroll the gang in the current global transaction?
	 */

	
	if (gp == NULL)
	{
		elog(ERROR, "No primary gang allocated");
	}

	if (gp != NULL)
	{
		gp->allocated = true;

		/* sanity check the gang */
		if (!gangOK(gp))
			elog(ERROR, "Segment connection failed: allocatePrimaryWriterGang attempted to return a bad gang.");
	}

	return gp;
}

Gang *
allocateMirrorWriterGang(void)
{
	/*
	 * First, we look for an unallocated but created gang of the right type
	 * if it exists, we return it.
	 * Else, we create a new gang
	 */

	MemoryContext oldContext;

	Gang	   *gp = NULL;

	Assert(Gp_role == GP_ROLE_DISPATCH);

	if (mirrorWriterGang == NULL && !mirroringNotConfigured)
	{
		int		nsegdb;
		
		if (!IsTransactionOrTransactionBlock())
		{
			elog(ERROR, "allocateMirrorWriterGang must be called inside of transaction");
		}
		
		if (GangContext == NULL)
		{ 
			GangContext = AllocSetContextCreate(TopMemoryContext,
												 "Gang Context",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);
		}
		Assert(GangContext != NULL);

		oldContext = MemoryContextSwitchTo(GangContext);
		
		nsegdb = getgpsegmentCount();
		/*
		 * try twice for creating gang
		 */
		gp = mirrorWriterGang = createGang(GANGTYPE_MIRROR_WRITER, 2, nsegdb, -1, NULL);
		if (gp == NULL && !mirroringNotConfigured)
			gp = mirrorWriterGang = createGang(GANGTYPE_MIRROR_WRITER, 2, nsegdb, -1, NULL);
		
		MemoryContextSwitchTo(oldContext);
	}
	else
	{
        if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
		    elog(DEBUG4, "Reusing an existing mirror writer gang");
		gp = mirrorWriterGang;
	}
	
	/*
	 * now we enroll the gang in the current global transaction?
	 */

	
	if (gp == NULL)
	{
		if (!mirroringNotConfigured)
			elog(ERROR, "No mirror gang allocated");
		
		return NULL;
	}


	gp->allocated = true;

	/* sanity check the gang */
	if (!gangOK(gp))
		elog(ERROR, "Segment connection failed: allocateMirrorWriterGang attempted to return a bad gang.");

	return gp;
}
/*
 * When we are the dispatch agent, we get told which gang to use by its "gang_id"
 * We need to find the gang in our lists.
 * 
 * keeping the gangs in the "available" lists on the Dispatch Agent is a hack,
 * as the dispatch agent doesn't differentiate allocated from available, or 1 gangs
 * from n-gangs.  It assumes the QD keeps track of all that.
 * 
 * It might be nice to pass gang type and size to this routine as safety checks.
 * 
 */

Gang * findGangById(int gang_id)
{
	if (primaryWriterGang && primaryWriterGang->gang_id == gang_id)
		return primaryWriterGang;
	if (mirrorWriterGang && mirrorWriterGang->gang_id == gang_id)
		return mirrorWriterGang;
	
	/*
	 * Now we iterate through the list of reader gangs
	 * to find the one that matches
	 */
	if (availableReaderGangsN != NIL)
	{
		ListCell   *cur_item;
		ListCell   *prev_item = NULL;
		
		cur_item = list_head(availableReaderGangsN);
		
		while (cur_item != NULL)
		{
			Gang	   *gp = (Gang *) lfirst(cur_item);
			
			if (gp && gp->gang_id == gang_id)
			{
				return gp;
			}
		
			/* cur_item must be preserved */
			prev_item = cur_item;
			cur_item = lnext(prev_item);
			 
		}
	}
	if (availableReaderGangs1 != NIL)
	{
		ListCell   *cur_item;
		ListCell   *prev_item = NULL;
		
		cur_item = list_head(availableReaderGangs1);
		
		while (cur_item != NULL)
		{
			Gang	   *gp = (Gang *) lfirst(cur_item);
			
			if (gp && gp->gang_id == gang_id)
			{
				return gp;
			}
		
			/* cur_item must be preserved */
			prev_item = cur_item;
			cur_item = lnext(prev_item);
			 
		}
	}
	
	/*
	 * 1-gangs can exist on some dispatch agents, and not on others.
	 * 
	 * so, we can't tell if not finding the gang is an error or not.
	 * 
	 * It would be good if we knew if this was a 1-gang on not.
	 * 
	 * The writer gangs are always n-gangs.
	 * 
	 */
	
	if (gang_id <= 2)
		elog(ERROR,"Could not find gang_id %d",gang_id);
	else
		elog(DEBUG1,"Could not find gang_id %d", gang_id);
	
	return NULL;
 
	
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
		{
			/*
			 * If we are using dispatch agents, return the segdb descriptor for the DA.
			 */
			if (gp->dispatchagents && gp->db_descriptors[i].myAgent)
				return (gp->db_descriptors[i].myAgent);
			return &(gp->db_descriptors[i]);
		}
	}

	return NULL;
}

List *
getCdbProcessList(Gang *gang, int sliceIndex)
{
	int			i;
	List	   *list = NIL;


	Assert((Gp_role == GP_ROLE_DISPATCH || Gp_role == GP_ROLE_DISPATCHAGENT) && Gp_is_primary);
	Assert(gang != NULL);
    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
	    elog(DEBUG3, "getCdbProcessList slice%d gangtype=%d gangsize=%d",
             sliceIndex, gang->type, gang->size);

	if (gang != NULL && gang->type != GANGTYPE_UNALLOCATED)
	{
		CdbComponentDatabaseInfo *qeinfo;
		bool		primary;
		int			k = 0;
		int			listsize = 0;

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
			if (gang->size > 1 ||
                gang->type == GANGTYPE_PRIMARY_WRITER ||
                gang->type == GANGTYPE_MIRROR_WRITER)
            {
                while (segdbDesc->segindex > listsize)
				{
					list = lappend(list, NULL);
					listsize++;
				}
            }

			process->listenerAddr = pstrdup(qeinfo->hostip);

			if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
				process->listenerPort = (segdbDesc->motionListener >> 16) & 0x0ffff;
			else
				process->listenerPort = (segdbDesc->motionListener & 0x0ffff);

			process->pid = segdbDesc->backendPid;
			process->contentid = segdbDesc->segindex;
			process->routeid = k++;

            if (gp_log_gang >= GPVARS_VERBOSITY_VERBOSE || DEBUG4 >= log_min_messages)
                elog(LOG, "Gang assignment (gang_id %d): slice%d %s%d %s:%d pid=%d",
                	 gang->gang_id,
                     sliceIndex,
                     gang->type == GANGTYPE_MIRROR_WRITER ? "mir" : "seg",
				     process->contentid,
                     process->listenerAddr,
                     process->listenerPort,
                     process->pid);

			list = lappend(list, process);
			listsize++;
		}

		if (gang->type == GANGTYPE_PRIMARY_WRITER &&
            listsize < getgpsegmentCount())
			elog(ERROR, "Primary gang smaller than number of segments");

		if (gang->size > 1 ||
            gang->type == GANGTYPE_PRIMARY_WRITER ||
            gang->type == GANGTYPE_MIRROR_WRITER)
		{
			while (listsize < getgpsegmentCount())
			{
				list = lappend(list, NULL);
				listsize++;
			}
		}
		Assert(listsize == 1 || listsize == getgpsegmentCount());
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

	CdbComponentDatabaseInfo *qdinfo;
	CdbProcess *proc;

	Assert((Gp_role == GP_ROLE_DISPATCH) && Gp_is_primary);
	if (!isPrimary)
	{
		elog(FATAL, "getCdbProcessesForQD: unsupported request for QD mirror process");
	}

	if (cdb_component_dbs == NULL)
    {
		cdb_component_dbs = getCdbComponentDatabases();
        if (cdb_component_dbs == NULL)
    		elog(ERROR, PACKAGE_NAME " schema not populated");
	}

	qdinfo = &(cdb_component_dbs->entry_db_info[0]);

	Assert(qdinfo->segindex == -1 && qdinfo->isprimary);
	Assert(qdinfo->hostip != NULL);

	proc = makeNode(CdbProcess);
	proc->listenerAddr = pstrdup(qdinfo->hostip);
	if (Gp_interconnect_type == INTERCONNECT_TYPE_UDP)
		proc->listenerPort = (Gp_listener_port >> 16) & 0x0ffff;
	else 
		proc->listenerPort = (Gp_listener_port & 0x0ffff);
	proc->pid = MyProcPid;
	proc->contentid = -1;
	proc->routeid = 0;

	/*
	 * freeCdbComponentDatabases(cdb_component_dbs);
	 */
	list = lappend(list, proc);
	return list;
}

/*
 * cleanupGang():
 *
 * A return value of "true" means that the gang was intact (or NULL).
 *
 * A return value of false, means that a problem was detected and the
 * gang has been disconnected (and so should not be put back onto the
 * available list).
 */
bool
cleanupGang(Gang * gp)
{
	int			i;

	if (gp == NULL)
		return true;
	
	elog(DEBUG5,"cleaning up gang %d",gp->gang_id);

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "cleanupGang: cleaning gang type %d size %d, was used for portal: %s", gp->type, gp->size, (gp->portal_name ? gp->portal_name : "(unnamed)"));

	/* disassociate this gang with any portal that it may have belonged to */
	if (gp->portal_name)
		pfree(gp->portal_name);
	
	gp->portal_name = NULL;
	
	if (gp->active)
		elog(DEBUG2, "cleanupGang called on a gang that is active");

	/*
	 * if the process is in the middle of blowing up... then we don't do
	 * anything here.  making libpq and other calls can definitely result in
	 * things getting HUNG.
	 */
	if (proc_exit_inprogress)
		return true;
	

	if (Gp_role == GP_ROLE_DISPATCH && gp_use_dispatch_agent && gp->dispatchagents)
	{
		int i;

		for (i = 0; i < gp->dispatchagents->size; i++)
		{
			if (PQstatus(gp->dispatchagents->db_descriptors[i].conn) == CONNECTION_OK)
			{
#if 0
				PGresult * result;
				elog(DEBUG1,"Sending clean gang message to dispatch agent on segdb %d",gp->dispatchagents->db_descriptors[i].segindex);
				PQsendControlGang(gp->dispatchagents->db_descriptors[i].conn, gp->gang_id, "clean gang");
				
				result = PQgetResult(gp->dispatchagents->db_descriptors[i].conn);
				
				if (PQresultStatus(result)!=PGRES_COMMAND_OK)
				{
					elog(WARNING,"Dispatch Agent unable to clean gang, %s %s",PQresStatus(PQresultStatus(result)),
							PQresultErrorMessage(result));
					return false;
				}
#endif
			}
			else
			{
				elog(LOG, "cleanupGang: lost connection with dispatch agent. Disconnecting");
				disconnectAndDestroyGang(gp);
				return false;
			}
		}
	}


	/*
	 * Loop through the segment_database_descriptors array and, for each
	 * SegmentDatabaseDescriptor:
	 *	   1) discard the query results (if any)
	 */
	for (i = 0; i < gp->size; i++)
	{
		SegmentDatabaseDescriptor *segdbDesc = &(gp->db_descriptors[i]);
		if (segdbDesc == NULL)
			return false;

		/*
		 * Note, we cancel all "still running" queries
		 */
		/* PQstatus() is smart enough to handle NULL */
		
		if (!gp_use_dispatch_agent || Gp_role == GP_ROLE_DISPATCHAGENT)
		{
		if (PQstatus(segdbDesc->conn) == CONNECTION_OK)
		{
			PGresult   *pRes;
			int			retry = 0;

			ExecStatusType stat;

			while (NULL != (pRes = PQgetResult(segdbDesc->conn)))
			{
				stat = PQresultStatus(pRes);

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
					elog(LOG, "cleanupGang called when a gang member is still busy, waiting didn't help, trying to destroy the gang");
					disconnectAndDestroyGang(gp);
					gp = NULL;
					elog(FATAL, "cleanupGang called when a gang member is still busy.  Fatal error.");
					return false;
				}
			}
		}
		else
		{
			/* something happened to this gang, disconnect instead of sending it back onto our lists! */
			elog(LOG, "cleanupGang: lost connection with gang member. Disconnecting");
			disconnectAndDestroyGang(gp);
			return false;
		}
		}

		/* QE is no longer associated with a slice. */
		cdbconn_setSliceIndex(segdbDesc, -1);
	}

	gp->allocated = false;

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
	    elog(DEBUG5, "cleanupGang done");
	return true;
}

/*
 * This not only disconnects and destroys the gang, it also first removes the gang from
 * the available lists (if it exists in those lists).
 */

void
disconnectAndDestroyGangById(int gang_id)
{
	MemoryContext oldContext;
	
	Assert(GangContext != NULL);
	oldContext = MemoryContextSwitchTo(GangContext);

	/*
	 * Now we iterate through the list of available reader gangs
	 * and we destroy the gang that was specified.
	 */
	if (availableReaderGangsN != NIL)
	{
		ListCell   *cur_item;
		ListCell   *prev_item = NULL;
		
		cur_item = list_head(availableReaderGangsN);
		
		while (cur_item != NULL)
		{
			Gang	   *gp = (Gang *) lfirst(cur_item);
			
			if (gp->gang_id == gang_id)
			{
                		
				/* cur_item must be removed */
				availableReaderGangsN = list_delete_cell(availableReaderGangsN, cur_item, prev_item);
				
				disconnectAndDestroyGang(gp);
				
				if (prev_item)
					cur_item = lnext(prev_item);
				else
					cur_item = list_head(availableReaderGangsN);
			}
			else
			{
				
				/* cur_item must be preserved */
				prev_item = cur_item;
				cur_item = lnext(prev_item);
			}
		}
	}
	
	if (availableReaderGangs1 != NIL)
	{
		ListCell   *cur_item;
		ListCell   *prev_item = NULL;
		
		cur_item = list_head(availableReaderGangs1);
		
		while (cur_item != NULL)
		{
			Gang	   *gp = (Gang *) lfirst(cur_item);
			
			if (gp->gang_id == gang_id)
			{
				
				/* cur_item must be removed */
				availableReaderGangs1 = list_delete_cell(availableReaderGangs1, cur_item, prev_item);
				
				disconnectAndDestroyGang(gp);
				
				if (prev_item)
					cur_item = lnext(prev_item);
				else
					cur_item = list_head(availableReaderGangs1);
			}
			else
			{
				prev_item = cur_item;
				cur_item = lnext(prev_item);
			}
		}
	}
	
	MemoryContextSwitchTo(oldContext);

    if (primaryWriterGang && primaryWriterGang->gang_id == gang_id)
    {
    	disconnectAndDestroyGang(primaryWriterGang);
    	primaryWriterGang = NULL;
    }
    else if (mirrorWriterGang && mirrorWriterGang->gang_id == gang_id)
    {
    	disconnectAndDestroyGang(mirrorWriterGang);
    	mirrorWriterGang = NULL;
    }
}

void
disconnectAndDestroyGang(Gang * gp)
{
	int			i;

	if (gp == NULL)
		return;

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
	    elog(DEBUG5, "disconnectAndDestroyGang entered");

	if (gp->active ||
		gp->allocated)
		elog(DEBUG2, "Warning: disconnectAndDestroyGang called on an %s gang",
			 gp->active ? "active" : "allocated");
	
	if (gp->gang_id < 1 || gp->gang_id > 100000000 || gp->type > 10 || gp->size > 100000)
	{
		elog(LOG,"disconnectAndDestroyGang on bad gang");
		return;
	}
	
	if (Gp_role == GP_ROLE_DISPATCH && gp_use_dispatch_agent && gp->dispatchagents)
	{
		PGresult * result;
		int i;

		for (i = 0; i < gp->dispatchagents->size; i++)
		{
			PQsendControlGang(gp->dispatchagents->db_descriptors[i].conn, gp->gang_id, "destroy gang");
			
			result = PQgetResult(gp->dispatchagents->db_descriptors[i].conn);
			
			if (PQresultStatus(result)!=PGRES_COMMAND_OK)
			{
				elog(ERROR,"Dispatch Agent unable to destroy gang, %s %s",PQresStatus(PQresultStatus(result)),
						PQresultErrorMessage(result));
			}
		}
	}

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
		if (segdbDesc == NULL)
			continue;

		if (segdbDesc->conn && PQstatus(segdbDesc->conn) != CONNECTION_BAD)
		{
			
			PGTransactionStatusType status =
				PQtransactionStatus(segdbDesc->conn);

			elog((Debug_print_full_dtm ? LOG : DEBUG5),"disconnectAndDestroyGang: got QEDistributedTransactionId = %u, QECommandId = %u, and QEDirty = %s",
			     segdbDesc->conn->QEWriter_DistributedTransactionId, 
			     segdbDesc->conn->QEWriter_CommandId, (segdbDesc->conn->QEWriter_Dirty ? "true" : "false"));

            if (gp_log_gang >= GPVARS_VERBOSITY_TERSE)
            {
                const char *ts;

                switch (status)
                {
                    case PQTRANS_IDLE:
                        ts = "idle";
                        break;
                    case PQTRANS_ACTIVE:
                        ts = "active";
                        break;
                    case PQTRANS_INTRANS:
                        ts = "idle, within transaction";
                        break;
                    case PQTRANS_INERROR:
                        ts = "idle, within failed transaction";
                        break;
                    case PQTRANS_UNKNOWN:
                        ts = "unknown transaction status";
                        break;
                    default:
                        ts = "invalid transaction status";
                        break;
                }
                elog(DEBUG3, "Finishing connection with %s; %s",
				     segdbDesc->whoami, ts);
            }

			if (status == PQTRANS_ACTIVE)
			{
				char errbuf[256];
				PGcancel * cn = PQgetCancel(segdbDesc->conn);
				if (PQcancel(cn, errbuf, 256)==0)
				{	
					elog(LOG,"Unable to cancel %s: %s", segdbDesc->whoami, errbuf);
				}
				PQfreeCancel(cn);
			}

			PQfinish(segdbDesc->conn);

			segdbDesc->conn = NULL;
		}

		/* Free memory owned by the segdbDesc. */
		cdbconn_termSegmentDescriptor(segdbDesc);
	}


	/*
	 * when we get rid of the primary writer gang we MUST also get rid of the reader
	 * gangs due to the shared local snapshot code that is shared between
	 * readers and writers.
	 */
	if (gp->type == GANGTYPE_PRIMARY_WRITER)
	{
		disconnectAndDestroyAllReaderGangs();
		if (mirrorWriterGang)
		{
			disconnectAndDestroyGang(mirrorWriterGang);
			mirrorWriterGang = NULL;
		}
	}


	/*
	 * Discard the segment array and the cluster descriptor
	 */
	pfree(gp->db_descriptors);
	gp->db_descriptors = NULL;
	gp->size = 0;
	if(gp->portal_name != NULL)
		pfree(gp->portal_name);
	pfree(gp);

	/*
	 * this is confusing, gp is local variable, no need to null it. gp = NULL;
	 */
    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG5, "disconnectAndDestroyGang done");
}

/*
 * freeGangsForPortal
 *
 * Free all gangs that were allocated for a specific portal
 * (could either be a cursor name or an unnamed portal)
 *
 * Be careful when moving gangs onto the available list, if
 * cleanupGang() tells us that the gang has a problem, the gang has
 * been free()ed and we should discard it -- otherwise it is good as
 * far as we can tell.
 */
void
freeGangsForPortal(char *portal_name)
{
	MemoryContext oldContext;

	if (Gp_role != GP_ROLE_DISPATCH)
		return;	

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG4, "freeGangsForPortal '%s'. Reader gang inventory: "
             "allocatedN=%d availableN=%d allocated1=%d available1=%d",
	    	 (portal_name ? portal_name : "unnamed portal"),
             list_length(allocatedReaderGangsN), list_length(availableReaderGangsN),
		     list_length(allocatedReaderGangs1), list_length(availableReaderGangs1));

	/* the primary and mirror writer gangs "belong" to the unnamed portal -- if we have multiple active
	 * portals trying to release, we can't just release and re-release the writers each time ! */
	if (!portal_name)
	{
		if (!cleanupGang(primaryWriterGang))
		{
			primaryWriterGang = NULL; /* cleanupGang called disconnectAndDestroyGang() already */
			disconnectAndDestroyAllGangs();
			elog(ERROR, "freeGangsForPortal: Bad primary writer gang");
			return;
		}
		if (!cleanupGang(mirrorWriterGang))
		{
			mirrorWriterGang = NULL; /* cleanupGang called disconnectAndDestroyGang() already */
			disconnectAndDestroyAllGangs();
			elog(ERROR, "freeGangsForPortal: Bad mirror writer gang");
			return;
		}
	}
	
	if (GangContext)
		oldContext = MemoryContextSwitchTo(GangContext);
	else
		oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Now we iterate through the list of allocated reader gangs
	 * and we free all the gangs that belong to the portal that
	 * was specified by our caller.
	 */
	if (allocatedReaderGangsN != NIL)
	{
		ListCell   *cur_item;
		ListCell   *prev_item = NULL;
		
		cur_item = list_head(allocatedReaderGangsN);
		
		while (cur_item != NULL)
		{
			Gang	   *gp = (Gang *) lfirst(cur_item);
			
			if (isTargetPortal(gp->portal_name, portal_name))
			{
                if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
				    elog(DEBUG5, "Returning a reader N-gang to the available list");
				
				/* cur_item must be removed */
				allocatedReaderGangsN = list_delete_cell(allocatedReaderGangsN, cur_item, prev_item);
				
				/* we only return the gang to the available list if it is good */
				if (cleanupGang(gp))
					availableReaderGangsN = lappend(availableReaderGangsN, gp);
				
				if (prev_item)
					cur_item = lnext(prev_item);
				else
					cur_item = list_head(allocatedReaderGangsN);
			}
			else
			{
                if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
				    elog(DEBUG5, "Skipping the release of a reader N-gang. It is used by another portal");
				
				/* cur_item must be preserved */
				prev_item = cur_item;
				cur_item = lnext(prev_item);
			}
		}
	}
	
	if (allocatedReaderGangs1 != NIL)
	{
		ListCell   *cur_item;
		ListCell   *prev_item = NULL;
		
		cur_item = list_head(allocatedReaderGangs1);
		
		while (cur_item != NULL)
		{
			Gang	   *gp = (Gang *) lfirst(cur_item);
			
			if (isTargetPortal(gp->portal_name, portal_name))
			{
                if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
				    elog(DEBUG5, "Returning a reader 1-gang to the available list");
				
				/* cur_item must be removed */
				allocatedReaderGangs1 = list_delete_cell(allocatedReaderGangs1, cur_item, prev_item);
				
				/* we only return the gang to the available list if it is good */
				if (cleanupGang(gp))
					availableReaderGangs1 = lappend(availableReaderGangs1, gp);
				
				if (prev_item)
					cur_item = lnext(prev_item);
				else
					cur_item = list_head(allocatedReaderGangs1);
			}
			else
			{
                if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
					elog(DEBUG5, "Skipping the release of a reader 1-gang. It is used by another portal");
				
				/* cur_item must be preserved */
				prev_item = cur_item;
				cur_item = lnext(prev_item);
			}
		}
	}
	
	MemoryContextSwitchTo(oldContext);

    if (gp_log_gang >= GPVARS_VERBOSITY_VERBOSE)
    {
        if (allocatedReaderGangsN ||
            availableReaderGangsN ||
            allocatedReaderGangs1 ||
            availableReaderGangs1)
        {
		    elog(LOG, "Gangs released for portal '%s'. Reader gang inventory: "
                 "allocatedN=%d availableN=%d allocated1=%d available1=%d",
	    	     (portal_name ? portal_name : "unnamed portal"),
		         list_length(allocatedReaderGangsN), list_length(availableReaderGangsN),
		         list_length(allocatedReaderGangs1), list_length(availableReaderGangs1));
        }
    }
}

/*
 * disconnectAndDestroyAllReaderGangs
 *
 * Here we destroy all reader gangs regardless of the portal they belong to.
 * TODO: This may need to be done more carefully when multiple cursors are
 * enabled.
 */
static void
disconnectAndDestroyAllReaderGangs(void)
{
	Gang	   *gp;

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
			availableReaderGangs1 = list_delete_first(availableReaderGangs1);
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
}

void
disconnectAndDestroyDispatchAgents(void)
{
	int			i;

	if (dispatchagents == NULL)
		return;

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
	    elog(DEBUG5, "disconnectAndDestroyDispatchAgents entered");
    
    if (Gp_role != GP_ROLE_DISPATCH)
    	elog(ERROR,"Non-dispatch has dispatch agents?");

	/*
	 * do we want to send messages to the DA to say get rid of all gangs?
	 * or assume it has already been done by disconnectAndDestroyAllGangs?
	 * 
	{
		PGresult * result;
		int i;

		for (i = 0; i < dispatchagents->size; i++)
		{
			PQsendControlGang(dispatchagents->db_descriptors[i].conn, -1, "destroy gang");
			
			result = PQgetResult(dispatchagents->db_descriptors[i].conn);
			
			if (PQresultStatus(result)!=PGRES_COMMAND_OK)
			{
				elog(ERROR,"Dispatch Agent unable to destroy gang, %s %s",PQresStatus(PQresultStatus(result)),
						PQresultErrorMessage(result));
			}
		}
	}
	*/

	/*
	 * Loop through the segment_database_descriptors array and, for each
	 * SegmentDatabaseDescriptor:
	 *	   1) discard the query results (if any),
	 *	   2) disconnect the session, and
	 *	   3) discard any connection error message.
	 */
	for (i = 0; i < dispatchagents->size; i++)
	{
		SegmentDatabaseDescriptor *segdbDesc = &(dispatchagents->db_descriptors[i]);
		if (segdbDesc == NULL)
			continue;

		if (segdbDesc->conn && PQstatus(segdbDesc->conn) != CONNECTION_BAD)
		{
			
			PGTransactionStatusType status =
				PQtransactionStatus(segdbDesc->conn);

			elog((Debug_print_full_dtm ? LOG : DEBUG5),"disconnectAndDestroyDispatchAgents: got QEDistributedTransactionId = %u, QECommandId = %u, and QEDirty = %s",
			     segdbDesc->conn->QEWriter_DistributedTransactionId, 
			     segdbDesc->conn->QEWriter_CommandId, (segdbDesc->conn->QEWriter_Dirty ? "true" : "false"));

            if (gp_log_gang >= GPVARS_VERBOSITY_TERSE)
            {
                const char *ts;

                switch (status)
                {
                    case PQTRANS_IDLE:
                        ts = "idle";
                        break;
                    case PQTRANS_ACTIVE:
                        ts = "active";
                        break;
                    case PQTRANS_INTRANS:
                        ts = "idle, within transaction";
                        break;
                    case PQTRANS_INERROR:
                        ts = "idle, within failed transaction";
                        break;
                    case PQTRANS_UNKNOWN:
                        ts = "unknown transaction status";
                        break;
                    default:
                        ts = "invalid transaction status";
                        break;
                }
                elog(DEBUG3, "Finishing connection with %s; %s",
				     segdbDesc->whoami, ts);
            }

			if (status == PQTRANS_ACTIVE)
			{
				char errbuf[256];
				PGcancel * cn = PQgetCancel(segdbDesc->conn);
				if (PQcancel(cn, errbuf, 256)==0)
				{	
					elog(LOG,"Unable to cancel %s: %s", segdbDesc->whoami, errbuf);
				}
				PQfreeCancel(cn);
			}

			PQfinish(segdbDesc->conn);

			segdbDesc->conn = NULL;
		}

		/* Free memory owned by the segdbDesc. */
		cdbconn_termSegmentDescriptor(segdbDesc);
	}


	/*
	 * Discard the segment array and the cluster descriptor
	 */
	pfree(dispatchagents->db_descriptors);
	dispatchagents->db_descriptors = NULL;
	dispatchagents->size = 0;
	
	pfree(dispatchagents);
	
	dispatchagents = NULL;

    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
        elog(DEBUG5, "disconnectAndDestroyDispatchAgents done");
}

void
disconnectAndDestroyAllGangs(void)
{
    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG2, "disconnectAndDestroyAllGangs");

	/* for now, destroy all readers, regardless of the portal that owns them */
	disconnectAndDestroyAllReaderGangs();

	disconnectAndDestroyGang(mirrorWriterGang);
	mirrorWriterGang = NULL;
	
	disconnectAndDestroyGang(primaryWriterGang);
	primaryWriterGang = NULL;
	

	disconnectAndDestroyDispatchAgents();
	
    if (gp_log_gang >= GPVARS_VERBOSITY_DEBUG)
		elog(DEBUG3, "disconnectAndDestroyAllGangs done");
}

bool
gangOK(Gang * gp)
{
	int			i;

	if (gp == NULL)
		return false;
	
	if (gp->gang_id < 1 || gp->gang_id > 100000000 || gp->type > 10 || gp->size > 100000)
		return false;

	/*
	 * Loop through the segment_database_descriptors array looking for
	 * invalid connections
	 */
	if (!gp_use_dispatch_agent || Gp_role == GP_ROLE_DISPATCHAGENT)
	{
		/*
		 * Gang is direct-connect (no agents).
		 */
	 
		for (i = 0; i < gp->size; i++)
		{
			SegmentDatabaseDescriptor *segdbDesc = &(gp->db_descriptors[i]);
	
			if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
				return false;
		}
	}
	else
	{
		/*
		 * Gang is indirect via dispatch agents.
		 */
	 
		for (i = 0; i < gp->dispatchagents->size; i++)
		{
			SegmentDatabaseDescriptor *segdbDesc = &(gp->dispatchagents->db_descriptors[i]);
			
			if (segdbDesc == NULL)
				return false;
	
			if (PQstatus(segdbDesc->conn) == CONNECTION_BAD)
				return false;
		}
		/*
		 * TODO:  Should we send a message to the dispatch agent, asking that it verify the
		 * integrity of the gang?
		 */
		
	}

	return true;
}

bool
reconnectSegDB(struct SegmentDatabaseDescriptor * failedDB)
{
	int			segdbs_in_thread_pool = 0;
	DoConnectParms *doConnectParmsAr;

	/*
	 * Someone has probably already taken care of any error info from the
	 * original connection attempt, but let's make sure.
	 */
	if (failedDB->error_message.len > 0)
	{
		ereport(LOG, (errcode(failedDB->errcode),
					  errmsg("Segment db connection error: %s",
							 failedDB->error_message.data)));
		failedDB->errcode = 0;
		resetPQExpBuffer(&failedDB->error_message);
	}

	/* The old PGconn object should have been freed already.  Just make sure. */
	if (failedDB->conn)
	{
		elog(DEBUG3, "reconnectSegDB calling PQfinish for %s", failedDB->whoami);
		PQfinish(failedDB->conn);
		failedDB->conn = NULL;
	}

	/**
	 ** Reconnect.
	 **/
	elog(LOG, "Attempting to reconnect to %s", failedDB->whoami);

	doConnectParmsAr = (DoConnectParms *) palloc0(sizeof(*doConnectParmsAr));

	addSegDBToConnThreadPool(doConnectParmsAr, &segdbs_in_thread_pool,
							 failedDB);

	thread_DoConnect((void *) doConnectParmsAr);

	pfree(doConnectParmsAr->segdbDescPtrArray);
	pfree(doConnectParmsAr);

	/*
	 * Check new connection.
	 * If unsuccessful, thread_DoConnect() has already stored an
	 * error message and freed the PGconn object.
	 */
	if (!failedDB->conn)
	{
		ereport(LOG, (errcode(failedDB->errcode),
					  errmsg("Could not reconnect to segment db.  %s",
							 failedDB->error_message.data)));
		failedDB->errcode = 0;
		resetPQExpBuffer(&failedDB->error_message);
		return false;
	}

	elog(LOG, "Reconnected to segment db: %s", failedDB->whoami);

	Assert(PQstatus(failedDB->conn) != CONNECTION_BAD);


	/*
	 * If we've made it this far, then that means our re-connection attempts
	 * are successful.	now, we need to blow away all of the reader gangs in
	 * case this is a writer gang member.  The reason we need to do this is
	 * because of shared snapshots.  A reader qExec can't be used after its
	 * writer qExec dies.
	 *
	 * TODO: try and determine if we are a member of a reader or writer gang.
	 * then we'd only have to do the following if we were a writer gang.
	 *
	 */
	disconnectAndDestroyAllReaderGangs();



	return true;
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

			if (!FtsTestConnection(segdbDesc->segment_database_info))
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

			if (!FtsTestConnection(segdbDesc->segment_database_info))
				markSegDBStatus(segdbDesc->segment_database_info->dbid, false);
		}
	}

}

CdbComponentDatabases *
getComponentDatabases(void)
{
	Assert(Gp_role == GP_ROLE_DISPATCH);
	return (cdb_component_dbs == NULL) ? getCdbComponentDatabases() : cdb_component_dbs;
}

bool
gangsExist(void)
{
	return (primaryWriterGang != NULL ||
			mirrorWriterGang != NULL ||
			allocatedReaderGangsN != NIL ||
			availableReaderGangsN != NIL ||
			allocatedReaderGangs1 != NIL ||
			availableReaderGangs1 != NIL);
}


/*
 * SetSeqServerInfo:  Adds sequence server information to the
 * supplied SliceTable (assumed uninitialized).
 *
 * NOTE: Though not strictly gang-related, the function lives
 * here to gain access to the interesting static variables.
 */
void
SetSeqServerInfo(struct QueryDesc *queryDesc)
{
	CdbComponentDatabaseInfo *qdinfo;
	EState	   *estate = queryDesc->estate;
	SliceTable *sliceTable = estate->es_sliceTable;

	Assert(IsA(sliceTable, SliceTable) && sliceTable->seqServerHost == NULL);

	if (Gp_role != GP_ROLE_DISPATCH || !Gp_is_primary)
	{
		elog(FATAL, "SetSeqServerInfo: unsupported request");
	}

	qdinfo = &(getComponentDatabases()->entry_db_info[0]);

	Assert(qdinfo->segindex == -1 && qdinfo->isprimary);

	Assert(qdinfo->hostip != NULL);

	sliceTable->seqServerHost = pstrdup(qdinfo->hostip);
	sliceTable->seqServerPort = seqServerCtl->seqServerPort;
}

/*
 * the gang is working for portal p1. we are only interested in gangs
 * from portal p2. if p1 and p2 are the same portal return true. false
 * otherwise.
 */
static
bool isTargetPortal(char *p1, char *p2)
{
	/* both are unnamed portals (represented as NULL) */
	if (!p1 && !p2)
		return true;
	
	/* one is unnamed, the other is named */
	if (!p1 || !p2)
		return false;
	
	/* both are the same named portal */
	if (strcmp(p1, p2) == 0)
		return true;
	
	return false;
}
