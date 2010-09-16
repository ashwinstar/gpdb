package model
{
	import command.QueryPlanCommand;
	
	import flash.events.TimerEvent;
	import flash.utils.Timer;
	
	import mx.collections.ArrayCollection;
	import mx.collections.Sort;
	import mx.collections.SortField;
	
	import util.Debug;
	import util.MiscUtils;
	
	import value.Query;
			
	[Bindable]
	public class Queries
	{
		public function Queries(_mdl:GPPerfMonModelLocator):void
		{
			mdl = _mdl;	
			
			// Setup the filter function to show only our queries
			// i.e. we have two ArrayCollections - one filtered and one not
			myQueriesNowArray.filterFunction = myQueriesFilter;
			
			// Setup the sort functions
			
			var runttimeSort:Sort = new Sort();
			var runtimeSortField:SortField = new SortField("time_run", false, true, true);
			runttimeSort.fields = new Array( runtimeSortField );
			queriesNowByRuntime.sort = runttimeSort;

			var cpuPctSort:Sort = new Sort();
			var cpuPctSortField:SortField = new SortField("cpu_currpct", false, true, true);
			cpuPctSort.fields = new Array( cpuPctSortField );
			queriesNowByCpuPct.sort = cpuPctSort;
			
			var cpuTotalSort:Sort = new Sort();
			var cpuTotalSortField:SortField = new SortField("cpu_elapsed", false, true, true);
			cpuTotalSort.fields = new Array( cpuTotalSortField );
			queriesNowByCpuTotal.sort = cpuTotalSort;

			var cpuSkewSort:Sort = new Sort();
			var cpuSkewSortField:SortField = new SortField("skew_cpu", false, true, true);
			cpuSkewSort.fields = new Array( cpuSkewSortField );
			queriesNowByCpuSkew.sort = cpuSkewSort;

			var rowSkewSort:Sort = new Sort();
			var rowSkewSortField:SortField = new SortField("skew_rows", false, true, true);
			rowSkewSort.fields = new Array( rowSkewSortField );
			queriesNowByRowSkew.sort = rowSkewSort;
		}
				
		public function queriesNow(queriesData:XML):void
		{
			var newArray:Array = new Array;
			
			var runCount:int = 0;
			var queuedCount:int = 0;
			var myRunCount:int = 0;
			var myQueuedCount:int = 0;
						
			for each (var queryXML:XML in queriesData.query)
			{
				var query:Query = new Query();
				
				query.id = queryXML.@id;
				query.sample_time = MiscUtils.parseDate(queryXML.sample_time);
				query.username = queryXML.username;
				query.tsubmit = MiscUtils.parseDate(queryXML.tsubmit);
				query.cost = queryXML.cost;
				query.cpu_currpct = queryXML.cpu_currpct;
				query.cpu_elapsed = queryXML.cpu_elapsed;
				query.cpu_elapsed /= 1000.0;
				query.db = queryXML.db;
					
				query.query_text = queryXML.query_text;
				if (query.query_text.substr(-3, 3) == '...')
				{
					// The query text is truncated. See if we already have
					// the full version...
					
					for each (var q:Query in queriesNowArray)
					{
						if (q.id == query.id)
						{
							query.query_text = q.query_text;
							break;
						}
					}
				}	
				
				query.rows_out = queryXML.rows_out;
				query.skew_cpu = queryXML.skew_cpu;
				query.skew_rows = queryXML.skew_rows;
				if (queryXML.status == 'submit')
				{
					query.status = 'queued';
					queuedCount++;
					if (query.username == mdl.session.username)
						myQueuedCount++;
				}
				else if (queryXML.status == 'start')
				{
					query.status = 'running';
				    runCount++;
					if (query.username == mdl.session.username)
						myRunCount++;					    
				}
				else
					query.status = queryXML.status;
				
				var tStart:Date = MiscUtils.parseDate(queryXML.tstart);
				if (tStart != null)
				{
					query.time_queued = (tStart.time - query.tsubmit.time) / 1000.0;
					var tFinish:Date = MiscUtils.parseDate(queryXML.tfinish);					
					if (tFinish != null) 
					{
						query.time_run = (tFinish.time - tStart.time) / 1000.0;
					} else {
						query.time_run = (query.sample_time.time - tStart.time) / 1000.0;						
					}
				} else {
					query.time_queued = (query.sample_time.time -
										 query.tsubmit.time) / 1000.0;	
					query.time_run = 0;					
				}
				//time_queued is the wait time, it shouldn't be lessthan zero.
				if(query.time_queued < 0)
				{
					query.time_queued = 0;
				}
				if (query.time_run > 0)
				{
					query.cpu_velocity = Math.round((query.cpu_elapsed*1000.0) / query.time_run)/1000.0;
				}
				else
					query.cpu_velocity = 0;


				// Now check for watched and detail queries that need to be updated
				
				// Check if it is the detail query we are looking at, and if so update it
				if (mdl.nav.detailQuery && mdl.nav.detailQueryIsActive)
				   if (query.id == mdl.nav.detailQuery.id)
				      mdl.nav.detailQuery = query;

				// Is the query being watched?
				var count:int = 0;
				for each (var item:Query in watchedQueriesArray)
				{
					if (item.id == query.id)
					{
						query.watched = true;
						watchedQueriesCounts[item.status]--;
						watchedQueriesCounts[query.status]++;
						watchedQueriesArray.removeItemAt(count);
						watchedQueriesArray.addItem(query);
						break;
					}
					count++;
				}

				// Now add the query to the array, and we're done for this loop
				if (queryXML.status == 'start' || queryXML.status == 'submit')
				{	
					// Add it to the array
					newArray.push(query);
				}
						   
			}
			// Set all the arrayCollections to point to the array
			queriesNowArray.source = newArray;
			myQueriesNowArray.source = newArray;
			queriesNowByRuntime.source = newArray;
			queriesNowByCpuPct.source = newArray;
			queriesNowByCpuTotal.source = newArray;
			queriesNowByCpuSkew.source = newArray;
			queriesNowByRowSkew.source = newArray;
			
			// Build the 'all queries' string to show in the title
			queryNowSummary = "<b>All Queries:</b> " +
							  "<font size='16'>" + runCount.toString() + "</font> Running, " +
			                  "<font size='16'>" + queuedCount.toString() + "</font> Queued";

			// Build the 'my queries' string to show in the title
			myQueryNowSummary = "<b>My Queries:</b> " +
			                  "<font size='16'>" + myRunCount.toString() + "</font> Running, " +
			                  "<font size='16'>" + myQueuedCount.toString() + "</font> Queued"               

			mdl.queries.updateWatchedQueriesSummary();
		}


		// Response to a request for additional query details
		public function queryDetails(queriesData:XML):void
		{
			var query:XML = queriesData.query[0];
			
			if (!query)
			{
				Debug.debug("queryDetails: No valid details in response");
				return;
			}
			
			if (!mdl.nav.detailQuery || (query.@id != mdl.nav.detailQuery.id))
			{
				Debug.debug("queryDetails: response doesn't match current detail query");
				return;
			}
			
			// If things look good, then just copy over the complete SQL text
			mdl.nav.detailQuery.query_text = MiscUtils.noLineFeed(query.query_text);	
		}

		
		// Result of a query search
		public function queriesSearch(queriesData:XML):void
		{
			var newArray:Array = new Array;
			
			var finishedCount:int = 0;
			var abortedCount:int = 0;
			var row_count:int = queriesData.@row_count;
			var hit_row_max:String = queriesData.@hit_row_max;
						
			for each (var queryXML:XML in queriesData.query)
			{
				if (queryXML.status == 'done' || queryXML.status == 'abort')
				{
					var query:Query = new Query();
					
					query.id = queryXML.@id;
					query.sample_time = MiscUtils.parseDate(queryXML.sample_time);
					query.username = queryXML.username;
					query.tsubmit = MiscUtils.parseDate(queryXML.tsubmit);
					query.cost = queryXML.cost;
					query.cpu_currpct = queryXML.cpu_currpct;
					query.cpu_elapsed = queryXML.cpu_elapsed
					query.cpu_elapsed /= 1000.0; 
					query.db = queryXML.db;
					query.query_text = MiscUtils.noLineFeed(queryXML.query_text);					
					query.rows_out = queryXML.rows_out;
					query.skew_cpu = queryXML.skew_cpu;
					query.skew_rows = queryXML.skew_rows;
					query.status = queryXML.status;
					if (queryXML.status == 'done')
						finishedCount++;
					else if (queryXML.status == 'abort')
					    abortedCount++;				    
					
					var tStart:Date = MiscUtils.parseDate(queryXML.tstart);
					if (tStart != null)
					{
						query.time_queued = (tStart.time - query.tsubmit.time) / 1000.0;
						var tFinish:Date = MiscUtils.parseDate(queryXML.tfinish);
						if (tFinish != null) 
						{
							query.time_run = (tFinish.time - tStart.time) / 1000.0;
						} else {
							query.time_run = (query.sample_time.time - tStart.time) / 1000.0;						
						}
					} else {
						query.time_queued = (query.sample_time.time -
											 query.tsubmit.time) / 1000.0;	
						query.time_run = 0;					
					}	
					if (query.time_run > 0)
						query.cpu_velocity = Math.round((query.cpu_elapsed*1000.0) / query.time_run)/1000.0;
					else
						query.cpu_velocity = 0;
					
					// Add it to the array
					newArray.push(query);
				}
			}
			// Set the arrayCollections to point to the array
			searchedQueriesArray.source = newArray;
	
			// Build the 'serched queries' string to show in the title
			searchedQueriesSummary = "<b>Search Results:</b> " +
							  "<font size='16'>" + finishedCount.toString() + "</font> Finished, " +
			                  "<font size='16'>" + abortedCount.toString() + "</font> Aborted";
			// Did we hit the row max?
			if (hit_row_max=='true')
			{
			   searchedQueriesSummary += 
			   		" <font size='10'>(Max of " + row_count.toString() + " rows returned)<font>"
			}
		}

		// Allows us to return an error messge in case of error
		public function queriesSearchFailed(code:String, message:String):void
		{
			searchedQueriesSummary = "<b>Error:</b> " + code + " - " + message;
			searchedQueriesArray.removeAll();
		}


		// Used to filter for our queries
		private function myQueriesFilter(query:Query):Boolean
		{
			return (query.username == mdl.session.username);
		}


		public function updateWatchedQueriesSummary():void
		{
			// Build the 'watched queries' string to show in the title
			watchedQueriesSummary = "<b>Watched Queries:</b> " +
				  "<font size='16'>" + watchedQueriesCounts['running'].toString() + "</font> Running, " +
                  "<font size='16'>" + watchedQueriesCounts['queued'].toString() + "</font> Queued, " + 
				  "<font size='16'>" + watchedQueriesCounts['done'].toString() + "</font> Completed, " +
			      "<font size='16'>" + watchedQueriesCounts['abort'].toString() + "</font> Aborted";				
		}

		
		// Start fetching the query plan for this query
		//  - if active and on the query page, then fetch every quantum
		public function doQueryPlan(query:Query):void
		{
			// First handle turning off the old request
			// - if stored data is not realtime, cache it in case we want it again immediately
			if (!query || (query.id != queryPlanID))
			{
				pauseQueryPlan();
				
				// Should we cache it?
				if (queryPlanID && !queryPlanRealtime && !queryPlans[queryPlanID])
					queryPlans[queryPlanID ] = queryPlanCurrent; 
				
				queryPlanCurrent = null;
				queryPlanID = '';
			}
			
			if (query)
			{
				// Are we calling for the current query
				if (query.id == queryPlanID)
					return;				
				
				// Now handle new requests where we already have the data
				if (queryPlans[query.id])
				{
					Debug.debug("doQueryPlan: matched cached plan for query " + query.id);
					queryPlanCurrent = queryPlans[query.id];
					return;
				}
		
				// Now handle other requests
				queryPlanID = query.id;
				
				if (query.status == 'running' || query.status == 'queued')
				{
					queryPlanRealtime = true;
					// Start the per-quantum requests
					resumeQueryPlan();
				}
				else
				{
					queryPlanRealtime = false;
					
					// Issue a single request to get the data					
					var queryPlanCommand:QueryPlanCommand = new QueryPlanCommand();
					queryPlanCommand.setParameters(queryPlanID, false,
											query.tsubmit, query.sample_time);
					queryPlanCommand.execute();
				} 
			}
		}		

		// Pause requests for the query plan
		//  - navigated away from the page
		public function pauseQueryPlan():void
		{
			if (mdl.session.queryPlanTimer)
			{
				// Stop the quantum timer
				mdl.session.queryPlanTimer.stop();
				mdl.session.queryPlanTimer = null;
			}
		}
			
		// Resume requests for the query plan
		//  - navigated back to the page
		public function resumeQueryPlan():void
		{
			if (mdl.session.queryPlanTimer)
			   return;
			
			if ((queryPlanID != '') && queryPlanRealtime)
			{
				// First call it once for good measure
				onQueryPlanTimer(null);
				mdl.session.queryPlanTimer = new Timer( mdl.session.monitorQuantum * 1000);
				mdl.session.queryPlanTimer.addEventListener(TimerEvent.TIMER, onQueryPlanTimer);
				mdl.session.queryPlanTimer.start();				
			}
		}


		// Called every quantum tick to fetch query plan
		public function onQueryPlanTimer(event:TimerEvent):void
		{
			// Check if we are still running
			var isRunning:Boolean = false;
			for each (var q:Query in queriesNowArray)
			{
				if (q.id == queryPlanID)
				{
					isRunning = true;
					break;
				}
			}
			queryPlanRealtime = isRunning;

			var queryPlanCommand:QueryPlanCommand = new QueryPlanCommand();
			
			// Stop the timer if it isn't running any more
			if (!queryPlanRealtime)
			{
				pauseQueryPlan();
				// Search for +/- 5 mins of the latest tick
				queryPlanCommand.setParameters(queryPlanID, false,
						new Date(mdl.session.mostRecentTick.time-(MiscUtils.MINUTE*5)),
						new Date(mdl.session.mostRecentTick.time+(MiscUtils.MINUTE*5)));
			}
			else
				queryPlanCommand.setParameters(queryPlanID, true);
		    
			queryPlanCommand.execute(); 
		}

		// The result from our command call
		public function queryPlanResult(result:XML):void
		{
			var returnID:String = result.@id;
			if (returnID && (returnID != queryPlanID))
			{
				Debug.debug("queryPlan: Dropping result for old request (ID "+result.@id+")");
			}	
			else
			{
				// Set the value
				queryPlanCurrent = result;
			}
		}
		

		//==============================================

		// The currently selected query ID and index
		public var selectedQueryID:String = '';
		
		// The summary of all currently running queries -- for the title 
		public var queryNowSummary:String = '';
		// The summary of my currently running queries -- for the title 
		public var myQueryNowSummary:String = '';
		// The summary of my watched queries -- for the title 
		public var watchedQueriesSummary:String = '';	
		// The summary of my searched queries -- for the title 
		public var searchedQueriesSummary:String = '';
		
		// Stores the list of now queries
		public var queriesNowArray:ArrayCollection = new ArrayCollection();		
		// Stores the list of now queries - with our username 
		public var myQueriesNowArray:ArrayCollection = new ArrayCollection();
		// Stores the list of watched queries
		public var watchedQueriesArray:ArrayCollection = new ArrayCollection();		
		// Stores the list of searched for queries
		public var searchedQueriesArray:ArrayCollection = new ArrayCollection();
		
		// Always points to the currently Selected Queries DS.
		//public var currentQueriesArray:ArrayCollection = queriesNowArray;
		
		// Keeps track of counts of watched queries
		public var watchedQueriesCounts:Object = { queued:0, running:0, done:0, abort:0 };
		
		// Lookup to find the appropriate array for each tab
		public var queryArrayLookup:ArrayCollection = new ArrayCollection([
			queriesNowArray, myQueriesNowArray, 
			watchedQueriesArray, searchedQueriesArray, null]); 
		
		// The query ID that we are fetching the query plan for
		public var queryPlanID:String = '';
		public var queryPlans:Object = new Object();
		public var queryPlanCurrent:XML = null;
		public var queryPlanRealtime:Boolean = false;


		// Additional arrayCollections with sort orders applied
		public var queriesNowByRuntime:ArrayCollection = new ArrayCollection();
		public var queriesNowByCpuPct:ArrayCollection = new ArrayCollection();		
		public var queriesNowByCpuTotal:ArrayCollection = new ArrayCollection();
		public var queriesNowByCpuSkew:ArrayCollection = new ArrayCollection();
		public var queriesNowByRowSkew:ArrayCollection = new ArrayCollection();
				
		// Reference to the model
		private var mdl:GPPerfMonModelLocator;			
	}
}
