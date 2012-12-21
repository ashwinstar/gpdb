package model
{
	import command.QueryDetailsCommand;
	
	import value.DatabaseSample;
	import value.Query;
	import value.SystemSample;
	import mx.collections.ArrayCollection;
	
	[Bindable]
	public class NavState
	{
		// What main page are we on?
		public var mainPageIndex:int = 0;
		
		// Duration control for system metrics view
		public var durationList:Array = new Array(
			"1 hour", "6 hours", "24 hours", "7 days", 
			"2 weeks", "3 months", "1 year");
		public var durationSelected:int = 0;	

		// Which system tab page are we on
		public var systemTabPage:int = 0;

		// Which query tab page are we on
		public var queryTabPage:int = 0;

		// Which query detail tab page are we on
		public var queryDetailTabPage:int = 0;
		
		// Stores the selected data index
		public var selectedSystemIndex:int = -1;
		public var selectedSystemItem:SystemSample = null;					
		public var selectedDatabaseItem:DatabaseSample = null;	

		// Controls whether we are on summary view or detail view for a metric
		public var metricsList:ArrayCollection = new ArrayCollection([
			"Active Queries", "Memory", "CPU", "Load", 
			"Swap (in use)", "Swap (pages in/out)", 
			"Disk I/O (ops/s)", "Disk I/O (MB/s)", 
			"Network (pkts/s)", "Network (MB/s)"]);
		public var metricSelected:int = 0;
				
		// Which view on the query list page
		public var queryListToggle:Array = new Array(
			"Active - all queries", "Active - my queries", 
			"Watched queries", "Query search", "Query Details");
		
		// Which query ID and data provider do we wish to see details on?
		public var detailQuery:Query = null;
		public var detailQueryIsActive:Boolean = false;

		// Current query search
		public var qsearchSummary:String = "Click 'New Search' to search for queries";			
		
		// Jumps from anywhere in the app to this active query
		public function jumpToQueryDetails(q:Query, active:Boolean):void
		{
			if (q)
			{
				// Save the query we want details for
				detailQuery = q;
				// It is realtime, so we want updates
				detailQueryIsActive = active;
				// And toggle the main tabs to the query page
				mainPageIndex = 2;
				// On the query page we want the detail tab
				queryTabPage = 4;
				// Finally toggle the query detail page
				queryDetailTabPage = 0;
				
				// Check if the query text was shortened and requires an additional request
				if (q.query_text.substr(-3, 3) == '...')
				{
					var queryDetailsCommand:QueryDetailsCommand = new QueryDetailsCommand();
					if (detailQueryIsActive)
						queryDetailsCommand.setParameters(detailQuery.id, true);
					else
						queryDetailsCommand.setParameters(detailQuery.id, false,
													q.tsubmit, q.sample_time);
					queryDetailsCommand.execute();
				}
			}
		}
	}
}