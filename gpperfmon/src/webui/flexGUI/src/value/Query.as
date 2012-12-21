package value
{	
	[Bindable]
	public class Query
	{
		public var id:String = '';
		public var sample_time:Date = null;
		public var cost:Number = 0;
		public var cpu_currpct:Number = 0;
		public var cpu_elapsed:Number = 0;
		public var db:String = '';
		public var query_text:String = '';
		public var rows_out:Number = 0;
		public var skew_cpu:Number = 0;
		public var skew_rows:Number = 0;
		public var status:String = '';
		public var time_queued:Number = 0;
		public var time_run:Number = 0;
		public var cpu_velocity:Number = 0;
		public var tsubmit:Date = null;
		public var username:String = '';
		public var watched:Boolean = false;
	}
}