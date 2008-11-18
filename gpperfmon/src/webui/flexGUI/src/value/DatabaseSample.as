package value
{	
	[Bindable]
	public class DatabaseSample
	{
		public function divideBy(val:int):void
		{
			// Use to round to 3 decimal places
			var mult:Number = 1000.0/Number(val);
			
			queries_running = Math.round(queries_running*mult)/1000.0;
			queries_queued = Math.round(queries_queued*mult)/1000.0;
		}


		public function add(samp:DatabaseSample):void
		{
			queries_running += samp.queries_running;
			if (samp.queries_running_max > queries_running_max)
				queries_running_max = samp.queries_running_max;
			if (samp.queries_running_min < queries_running_min)
				queries_running_min = samp.queries_running_min;
			
			queries_queued += samp.queries_queued;
			if (samp.queries_queued_max > queries_queued_max)
				queries_queued_max = samp.queries_queued_max;
			if (samp.queries_queued_min < queries_queued_min)
				queries_queued_min = samp.queries_queued_min;		
		}

		// Updates the max values if necessary based on a new sample
		public function calcMax(samp:DatabaseSample):void
		{
			// Just use queries_running to hold the max
			
			if (samp.queries_running > queries_running)
				queries_running = samp.queries_running;
			if (samp.queries_running_max > queries_running)
				queries_running = samp.queries_running_max;			
				
			if (samp.queries_queued > queries_running)
				queries_running = samp.queries_queued;				
			if (samp.queries_queued_max > queries_running)
				queries_running = samp.queries_queued_max;														
		}
			
		public var sample_time:Date = null;
		public var queries_running:Number = 0;
		public var queries_running_min:Number = 0;
		public var queries_running_max:Number = 0;
		public var queries_queued:Number = 0;
		public var queries_queued_min:Number = 0;
		public var queries_queued_max:Number = 0;
	};
}