package model
{
	[Bindable]
	public class QueryList
	{
		// Current query search
		public var qsearchActive:int = 0;  
		public var qsearchSummary:String = "Click 'New Search' to search for queries";
		public var qsearchUsername:String = "";
		public var qsearchDatabase:String = "";
		public var qsearchStatus:String = "";
		public var qsearchStartTime:Number = -1;
		public var qsearchEndTime:Number = -1;
		public var qsearchMinTime:int = 0;
		public var qsearchQueryID:String = "";
		// Placeholder value
		public var qsearchResultSummary = 
			"<b>Results:</b> <font size='16'>0</font> Running, "+
			"<font size='16'>0</font> Queued, <font size='16'>130</font> Finished"+
			", <font size='16'>2</font> Aborted"; 
		public var qsearchResultSummary_old = 
			"<b>Results:</b> <font size='16'>0</font> Running, "+
			"<font size='16'>0</font> Queued, <font size='16'>130</font> Finished"+
			", <font size='16'>2</font> Aborted  <font size='10'>(Max of 250 rows returned)<font>"; 


	}
}