package model
{
	import command.SystemHistoricCommand;
	
	import mx.collections.ArrayCollection;
	import mx.events.ItemClickEvent;
	
	import util.Debug;
	import util.MiscUtils;
	
	import value.SystemSample;
		
	[Bindable]
	public class System
	{
		public function System(_mdl:GPPerfMonModelLocator):void
		{
			mdl = _mdl;	
		}
		
		public function systemNow(systemData:XML):void
		{
			systemNowXML = systemData;
			
			var newVals:SystemSample = new SystemSample;
			
			var count:int = 0;
			for each ( var host:XML in systemData.elements() )
			{				
				if (newVals.sample_time == null)
				{
					newVals.sample_time = MiscUtils.parseDate(host.sample_time);
					if (systemNow_lastReceivedDate && 
					    systemNow_lastReceivedDate.time >= newVals.sample_time.time)
					{
						return;
					}
					else
						systemNow_lastReceivedDate = newVals.sample_time;
				}
					
				var tmpVals:SystemSample = new SystemSample;
				
				tmpVals.cpu_idle = host.cpu_idle;
				tmpVals.cpu_sys = host.cpu_sys;
				tmpVals.cpu_user = host.cpu_user;
				
				tmpVals.disk_rb_rate = host.disk_rb_rate;
				tmpVals.disk_ro_rate = host.disk_ro_rate;
				tmpVals.disk_wb_rate = host.disk_wb_rate;
				tmpVals.disk_wo_rate = host.disk_wo_rate;
				
				tmpVals.load_0 = host.load0;
				tmpVals.load_1 = host.load1;
				tmpVals.load_2 = host.load2;	
				
				tmpVals.mem_actual_free = host.mem_actual_free;
				tmpVals.mem_actual_used = host.mem_actual_used;
				tmpVals.mem_total = host.mem_total;
				tmpVals.mem_used = host.mem_used;	
				
				tmpVals.net_rb_rate = host.net_rb_rate;
				tmpVals.net_rp_rate = host.net_rp_rate;
				tmpVals.net_wb_rate = host.net_wb_rate;
				tmpVals.net_wp_rate = host.net_wp_rate;									
					
				tmpVals.swap_page_in = host.swap_page_in;
				tmpVals.swap_page_out = host.swap_page_out;
				tmpVals.swap_total = host.swap_total;
				tmpVals.swap_used = host.swap_used;		
				
				newVals.add(tmpVals);
				
				count++;				
			}
			
			newVals.divideBy(count);
			
			systemNowAgg = newVals;		
			
			populateMinuteRollup();	
		}

		// Rolls up the latest samples into 1 minute buckets, and appends each 
		// completed minute to the oneMinAggData array	
		public function populateMinuteRollup():void
		{
			if (!systemNowAgg || systemNowAgg.sample_time == null)
			{
				Debug.debug("systemNow: populateMinuteRollup received null value");
				return;
			}
				
			var sampleMinute:Date = MiscUtils.date_trim_1min(systemNowAgg.sample_time);
			
			if (!systemNowCurrMinute || systemNowCurrMinute_numSamples == 0)
			{
				// Start the per-minute rollup
				
				systemNowCurrMinute = systemNowAgg;
				systemNowCurrMinute.sample_time = sampleMinute;
				systemNowCurrMinute_numSamples = 1;
			}
			else if (systemNowCurrMinute.sample_time.time != sampleMinute.time)
			{
				// Finish last per-minute sample and start a new one
				
				systemNowCurrMinute.divideBy(systemNowCurrMinute_numSamples);
		
				// Add this to the array
				if (!oneMin_lastReceivedDate || 
					(oneMin_lastReceivedDate.time < systemNowCurrMinute.sample_time.time))
				{
					oneMinAggData.addItem(systemNowCurrMinute);
				}	
				else
				{
					Debug.debug("Dropping real-time data for date " + 
							systemNowCurrMinute.sample_time.toString());
				}	
				
				// Restart the per-minute rollup
				systemNowCurrMinute = systemNowAgg;
				systemNowCurrMinute.sample_time = sampleMinute;
				systemNowCurrMinute_numSamples = 1;
			}
			else
			{
				// Add to the existing rollup
				
				systemNowCurrMinute.add(systemNowAgg);		
				
				systemNowCurrMinute_numSamples += 1;
			}
		}

		// Load historic system data
		public function systemHistoric(detail:String, start:Date, end:Date, systemData:XML):void
		{
			var elementArray:Array = new Array();
			var nameArray:Array = new Array();
			
			var maxLength:int = 0;
			var numColumns:int = 0;
			
			// Ensure the the array is empty
			if (detail == '1min')
				oneMinAggData.removeAll();
			else if (detail == '5min')
				fiveMinAggData.removeAll();
			else if (detail == '1hr')
				oneHrAggData.removeAll();
			else if (detail == '1day')
				oneDayAggData.removeAll();
			else
			{
				Debug.debug("Call to systemLoadHistoric with bad interval: " + detail);
				return;
			}
			
			// iterate through all elements in the parent hostname node.
			for each (var column:XML in systemData.* )
			{
				var valueArray:Array = column.toString().split('|');
				
				if (valueArray.length > maxLength)
				{
					maxLength = valueArray.length;
				}
				elementArray.push(valueArray);
				// Change 'load0/load1/load2' to 'load_0/load_1/load_2'
				if (column.name() == 'load0')
					nameArray.push('load_0');
				else if (column.name() == 'load1')
					nameArray.push('load_1'); 
				else if (column.name() == 'load2')
					nameArray.push('load_2');
				else
					nameArray.push(column.name());
			}
			numColumns = nameArray.length;
			
			for (var j:int=0; j<maxLength; j++)
			{
				// object to store system metrics for a single slice in time
				var obj:Object = new SystemSample();
				// Keep track of dates to make sure each is greater than the last
				var latestDate:Date = null;
				var thisDate:Date = null;
				// iterate through each element in a host node
				for (var i:int=0; i<numColumns; i++)
				{
					if (nameArray[i] == 'sample_time')
					{
						thisDate = MiscUtils.parseDate(elementArray[i][j]);
						obj[nameArray[i]] = thisDate;
					}
					else
					{
						obj[nameArray[i]] = Number(elementArray[i][j]);
					}					
				}
				if (!latestDate || (latestDate.time < thisDate.time))
				{
					latestDate = thisDate;
					if (detail == '1min')
						oneMinAggData.addItem(obj);
					else if (detail == '5min')
						fiveMinAggData.addItem(obj);
					else if (detail == '1hr')
						oneHrAggData.addItem(obj);
					else if (detail == '1day')
						oneDayAggData.addItem(obj);				
				}
				else
				{
					Debug.debug("Dropping historic data for date " + thisDate.toString());
				}
			}
			if (detail == '1min')
			{
				oneMin_firstReceivedDate = start;
				oneMin_lastReceivedDate = latestDate;
			}
			else if (detail == '5min')
			{
				fiveMin_firstReceivedDate = start;
				fiveMin_lastReceivedDate = latestDate;
			}
			else if (detail == '1hr')
			{
				oneHr_firstReceivedDate = start;
				oneHr_lastReceivedDate = latestDate;
			}
			else if (detail == '1day')
			{
				oneDay_firstReceivedDate = start;
				oneDay_lastReceivedDate = latestDate;				
			}	
		}

		// Called when the duration toggle is changed
		// -- use to trigger deferred data loading
		public function durationChange(event:ItemClickEvent):void
		{
			// Apply the change
			if (event.index >= 0)
				mdl.nav.durationSelected = event.index;
			
			if (mdl.system.activeAggLookup[event.index] == oneMinAggData)
			{
				// Shouldn't be here -- get 1min data on startup
				// -- could retry if failed, but skip for now
				if (!oneMin_lastReceivedDate)
					Debug.debug("Duration change: identified missing 1min data");
			} 
			else if (mdl.system.activeAggLookup[event.index] == fiveMinAggData)
			{
				if (!fiveMin_lastReceivedDate)
				{
					//Issue request to get the last 24hrs of 5min system data
					var fiveMinRequest:SystemHistoricCommand = new SystemHistoricCommand;
					fiveMinRequest.execute('5min', mdl.session.timeArray[Session.ONE_DAY_AGO]);	
				}
			}
			else if (mdl.system.activeAggLookup[event.index] == oneHrAggData)
			{
				if (!oneHr_lastReceivedDate)
				{
					//Issue request to get the last two weeks of 1hr system data
					var oneHrRequest:SystemHistoricCommand = new SystemHistoricCommand;
					oneHrRequest.execute('1hr', mdl.session.timeArray[Session.TWO_WEEKS_AGO]);	
				}
			}				
			else if (mdl.system.activeAggLookup[event.index] == oneDayAggData)
			{
				if (!oneDay_lastReceivedDate)
				{
					//Issue request to get the last 1yrs of 1day system data
					var oneDayRequest:SystemHistoricCommand = new SystemHistoricCommand;
					oneDayRequest.execute('1day', mdl.session.timeArray[Session.ONE_YEAR_AGO]);	
				}
			}			
		}

		// Stores the most recent system now info, aggregated across systems
		public var systemNowAgg:SystemSample = new SystemSample();
		// Stores the raw XML of the most recent system now response
		public var systemNowXML:XML = null;
		
		// Stores a running sum of the current minute's system information
		public var systemNowCurrMinute:SystemSample = null;
		// How many samples have gone into the current minute?
		public var systemNowCurrMinute_numSamples:int = 0;
		public var systemNow_lastReceivedDate:Date = null;
		
		// Stores last 6 hours at 1 minute granularity
		public var oneMinAggData:ArrayCollection = new ArrayCollection;
		// Last date for which we've received data
		public var oneMin_firstReceivedDate:Date = null;
		public var oneMin_lastReceivedDate:Date = null;

		// Stores last 24 hours at 5 minute granularity
		public var fiveMinAggData:ArrayCollection = new ArrayCollection;
		public var fiveMin_firstReceivedDate:Date = null;
		public var fiveMin_lastReceivedDate:Date = null;

		// Stores last 30 days at 1 hr granularity
		public var oneHrAggData:ArrayCollection = new ArrayCollection;	
		public var oneHr_firstReceivedDate:Date = null;
		public var oneHr_lastReceivedDate:Date = null;

		// Stores last 2 years at 1 day granularity 
		public var oneDayAggData:ArrayCollection = new ArrayCollection;			
		public var oneDay_firstReceivedDate:Date = null;
		public var oneDay_lastReceivedDate:Date = null;		
		
		// Lookup to find the appropriate data provide at each duration
		public var activeAggLookup:Array = new Array( null,
			oneMinAggData, oneMinAggData, fiveMinAggData, oneHrAggData, 
			oneHrAggData, oneDayAggData, oneDayAggData);	
		
		// Reference to the model
		private var mdl:GPPerfMonModelLocator;	
	}
}
