package value
{	
	[Bindable]
	public class SystemSample
	{
		public function divideBy(val:int):void
		{
			// Use to round to 3 decimal places
			var mult:Number = 1000.0/Number(val);
			
			cpu_idle = Math.round(cpu_idle*mult)/1000.0;
			cpu_sys = Math.round(cpu_sys*mult)/1000.0;
			cpu_user = Math.round(cpu_user*mult)/1000.0;
				
			disk_rb_rate = Math.round(disk_rb_rate*mult)/1000.0;	
			disk_ro_rate = Math.round(disk_ro_rate*mult)/1000.0;
			disk_wb_rate = Math.round(disk_wb_rate*mult)/1000.0;
			disk_wo_rate = Math.round(disk_wo_rate*mult)/1000.0;
				
			load_0 = Math.round(load_0*mult)/1000.0;
			load_1 = Math.round(load_1*mult)/1000.0;
			load_2 = Math.round(load_2*mult)/1000.0;

			mem_actual_free = Math.round(mem_actual_free*mult)/1000.0;	
			mem_actual_used = Math.round(mem_actual_used*mult)/1000.0;
			mem_total = Math.round(mem_total*mult)/1000.0;
			mem_used = Math.round(mem_used*mult)/1000.0;
				
			net_rb_rate = Math.round(net_rb_rate*mult)/1000.0;	
			net_rp_rate = Math.round(net_rp_rate*mult)/1000.0;
			net_wb_rate = Math.round(net_wb_rate*mult)/1000.0;
			net_wp_rate = Math.round(net_wp_rate*mult)/1000.0;								
					
			swap_page_in = Math.round(swap_page_in*mult)/1000.0;	
			swap_page_out = Math.round(swap_page_out*mult)/1000.0;
			swap_total = Math.round(swap_total*mult)/1000.0;
			swap_used = Math.round(swap_used*mult)/1000.0;			
		}

		
		public function add(samp:SystemSample):void
		{
			cpu_idle += samp.cpu_idle;
			cpu_sys += samp.cpu_sys;
			cpu_user += samp.cpu_user;
				
			disk_rb_rate += samp.disk_rb_rate;
			disk_ro_rate += samp.disk_ro_rate;
			disk_wb_rate += samp.disk_wb_rate;
			disk_wo_rate += samp.disk_wo_rate;
				
			load_0 += samp.load_0;
			load_1 += samp.load_1;
			load_2 += samp.load_2;	
				
			mem_actual_free += samp.mem_actual_free;
			mem_actual_used += samp.mem_actual_used;
			mem_total += samp.mem_total;
			mem_used += samp.mem_used;	
				
			net_rb_rate += samp.net_rb_rate;
			net_rp_rate += samp.net_rp_rate;
			net_wb_rate += samp.net_wb_rate;
			net_wp_rate += samp.net_wp_rate;									
				
			swap_page_in += samp.swap_page_in;
			swap_page_out += samp.swap_page_out;
			swap_total += samp.swap_total;
			swap_used += samp.swap_used;		
		}

		// Updates the max values if necessary based on a new sample
		public function calcMax(samp:SystemSample):void
		{
			// Just use disk_rb_rate to represent all disk vals
			if (samp.disk_rb_rate > disk_rb_rate)
			  disk_rb_rate = samp.disk_rb_rate;
			if (samp.disk_ro_rate > disk_rb_rate)
			  disk_rb_rate = samp.disk_ro_rate;
			if (samp.disk_wb_rate > disk_rb_rate)
			  disk_rb_rate = samp.disk_wb_rate;			  
			if (samp.disk_wo_rate > disk_rb_rate)
			  disk_rb_rate = samp.disk_wo_rate;
			  
			// Just use load_0 to represent all disk vals
			if (samp.load_0 > load_0)
				load_0 = samp.load_0;
			if (samp.load_1 > load_0)
				load_0 = samp.load_1;
			if (samp.load_2 > load_0)
				load_0 = samp.load_2;
				
			// Just use mem_total to represent all mem vals
			if ((samp.mem_actual_free+samp.mem_actual_used) > mem_total)
				mem_total = samp.mem_actual_free+samp.mem_actual_used;

			// Just use net_rb_rate to represent all net vals
			if (samp.net_rb_rate > net_rb_rate)
			  net_rb_rate = samp.net_rb_rate;
			if (samp.net_rp_rate > net_rb_rate)
			  net_rb_rate = samp.net_rp_rate;
			if (samp.net_wb_rate > net_rb_rate)
			  net_rb_rate = samp.net_wb_rate;			  
			if (samp.net_wp_rate > net_rb_rate)
			  net_rb_rate = samp.net_wp_rate;
				
			// Just use swap_page_in to represent all net vals
			if (samp.swap_page_in > swap_page_in)
				swap_page_in = samp.swap_page_in;				
			if (samp.swap_page_out > swap_page_in)
				swap_page_in = samp.swap_page_out;	

			// Just use swap_total to represent all swap vals
			if (samp.swap_total > swap_total)
				swap_total = samp.swap_total;	
		}			

				
		public var sample_time:Date = null;
		public var cpu_idle:Number = 0;
		public var cpu_sys:Number = 0;
		public var cpu_user:Number = 0;
		public var disk_rb_rate:Number = 0;
		public var disk_ro_rate:Number = 0;
		public var disk_wb_rate:Number = 0;
		public var disk_wo_rate:Number = 0;
		public var load_0:Number = 0;
		public var load_1:Number = 0;
		public var load_2:Number = 0;
		public var mem_actual_free:Number = 0;
		public var mem_actual_used:Number = 0;
		public var mem_total:Number = 0;
		public var mem_used:Number = 0;
		public var net_rb_rate:Number = 0;
		public var net_rp_rate:Number = 0;
		public var net_wb_rate:Number = 0;
		public var net_wp_rate:Number = 0;
		public var swap_page_in:Number = 0;
		public var swap_page_out:Number = 0;
		public var swap_total:Number = 0;
		public var swap_used:Number = 0; 
	};
}