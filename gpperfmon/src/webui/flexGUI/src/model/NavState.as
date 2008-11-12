package model
{
	[Bindable]
	public class NavState
	{
		// Duration control for system metrics view
		public var durationList:Array = new Array(
			"Now", "1 hour", "6 hours", "24 hours", "7 days", 
			"2 weeks", "3 months", "1 year");
		public var durationSelected:int = 1;	

		// Controls whether we are on summary view or detail view for a metric
		public var metricsList:Array = new Array(
			"Summary View",
			"Active Queries", "Memory", "CPU", "Load", 
			"Swap (in use)", "Swap (pages in/out)", 
			"Disk I/O (packets)", "Disk I/O (MBytes)", 
			"Network (packets)", "Network (MBytes)");
		public var metricSelected:int = 0;
				
		// Which view on the query list page
		public var queryListToggle:Array = new Array(
			"Active - all queries", "Active - my queries", "Watched queries", "Query search");
		public var queryListView:int = 0;	
		
		// Are we looking at the query details of a specific query?
		public var queryDetailID:int = -1;
		
		// Are we looking at the details of a specific query iterator?
		public var queryIteratorID:int = -1;
	}
}