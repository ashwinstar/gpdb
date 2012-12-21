package model
{
	import mx.charts.chartClasses.StackedSeries;
			
	[Bindable]
	public class HealthStatusCount {
		public var status:String = "";
		public var statusLabel:String = "";
		public var count:int = 0;
		public var nonEmpty:Boolean = false;
	}
}
