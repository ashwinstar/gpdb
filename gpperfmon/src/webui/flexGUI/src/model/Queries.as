package model
{
	import mx.collections.ArrayCollection;
	import mx.collections.Sort;
	import mx.collections.SortField;
	
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
			var runtimeSortField:SortField = new SortField("time_run");
			runtimeSortField.descending = true;
			runttimeSort.fields = new Array( runtimeSortField );
			queriesNowByRuntime.sort = runttimeSort;

			var cpuElapsedSort:Sort = new Sort();
			var cpuElapsedSortField:SortField = new SortField("cpu_elapsed");
			cpuElapsedSortField.descending = true;
			cpuElapsedSort.fields = new Array( cpuElapsedSortField );
			queriesNowByCpuUsage.sort = cpuElapsedSort;
			
			var cpuVelocitySort:Sort = new Sort();
			var cpuVelocitySortField:SortField = new SortField("cpu_velocity");
			cpuVelocitySortField.descending = true;
			cpuVelocitySort.fields = new Array( cpuVelocitySortField );
			queriesNowByCpuVelocity.sort = cpuVelocitySort;

			var cpuSkewSort:Sort = new Sort();
			var cpuSkewSortField:SortField = new SortField("skew_cpu");
			cpuSkewSortField.descending = true;
			cpuSkewSort.fields = new Array( cpuSkewSortField );
			queriesNowByCpuSkew.sort = cpuSkewSort;

			var rowSkewSort:Sort = new Sort();
			var rowSkewSortField:SortField = new SortField("skew_rows");
			rowSkewSortField.descending = true;
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
				if (queryXML.status == 'start' || queryXML.status == 'submit')
				{
					var query:Query = new Query();
					
					query.id = queryXML.@id;
					query.sample_time = MiscUtils.parseDate(queryXML.sample_time);
					query.username = queryXML.username;
					query.tsubmit = MiscUtils.parseDate(queryXML.tsubmit);
					query.cost = queryXML.cost;
					query.cpu_currpct = queryXML.cpu_currpct;
					query.cpu_elapsed = queryXML.cpu_elapsed;
					query.db = queryXML.db;
					query.query_text = queryXML.query_text;
					query.rows_out = queryXML.rows_out;
					query.skew_cpu = queryXML.skew_cpu;
					query.skew_rows = queryXML.skew_rows;
					query.status = queryXML.status;
					if (query.status == 'submit')
					{
						queuedCount++;
						if (query.username == mdl.session.username)
							myQueuedCount++;
					}
					else if (query.status == 'start')
					{
					    runCount++;
						if (query.username == mdl.session.username)
							myRunCount++;					    
					}
					
					if (queryXML.tstart != null)
					{
						query.time_queued = (MiscUtils.parseDate(queryXML.tstart).time -
											query.tsubmit.time) / 1000.0;
						if (queryXML.tfinish != null) 
						{
							query.time_run = (MiscUtils.parseDate(queryXML.tfinish).time -
								MiscUtils.parseDate(queryXML.tstart).time) / 1000.0;
						} else {
							query.time_run = (query.sample_time.time -
								MiscUtils.parseDate(queryXML.tstart).time) / 1000.0;						
						}
					} else {
						query.time_queued = (query.sample_time.time -
											 query.tsubmit.time) / 1000.0;	
						query.time_run = 0;					
					}	
					if (query.time_run > 0)
						query.cpu_velocity = query.cpu_elapsed / query.time_run;
					else
						query.cpu_velocity = 0;
					
					newArray.push(query);
				}
				else
				{
					// Handle queries that have finished
					
				}
			}
			// Set all the arrayCollections to point to the array
			queriesNowArray.source = newArray;
			myQueriesNowArray.source = newArray;
			queriesNowByRuntime.source = newArray;
			queriesNowByCpuUsage.source = newArray;
			queriesNowByCpuVelocity.source = newArray;
			queriesNowByCpuSkew.source = newArray;
			queriesNowByRowSkew.source = newArray;
			
			// Build the 'all queries' string to show in the title
			queryNowSummary = "<font size='16'>" + runCount.toString() + "</font> Running, " +
			                  "<font size='16'>" + queuedCount.toString() + "</font> Queued, " +
			                  "<font size='16'>0</font> Finished, " + 
			                  "<font size='16'>0</font> Aborted";

			// Build the 'my queries' string to show in the title
			myQueryNowSummary = "<font size='16'>" + myRunCount.toString() + "</font> Running, " +
			                  "<font size='16'>" + myQueuedCount.toString() + "</font> Queued, " +
			                  "<font size='16'>0</font> Finished, " + 
			                  "<font size='16'>0</font> Aborted";			                  
		}


		// Used to filter for our queries
		private function myQueriesFilter(query:Query):Boolean
		{
			return (query.username == mdl.session.username);
		}

		
		//==============================================

		// The currently selected query ID
		public var selectedQueryID:String = '';

		// The summary of all currently running queries -- for the title 
		public var queryNowSummary:String = '';
		// The summary of my currently running queries -- for the title 
		public var myQueryNowSummary:String = '';
		
		// Stores the list of now queries
		public var queriesNowArray:ArrayCollection = new ArrayCollection();		
		// Stores the list of now queries - with our username 
		public var myQueriesNowArray:ArrayCollection = new ArrayCollection();

		// Additional arrayCollections with sort orders applied
		public var queriesNowByRuntime:ArrayCollection = new ArrayCollection();
		public var queriesNowByCpuUsage:ArrayCollection = new ArrayCollection();		
		public var queriesNowByCpuVelocity:ArrayCollection = new ArrayCollection();
		public var queriesNowByCpuSkew:ArrayCollection = new ArrayCollection();
		public var queriesNowByRowSkew:ArrayCollection = new ArrayCollection();
				
		// Reference to the model
		private var mdl:GPPerfMonModelLocator;			
	}
}
