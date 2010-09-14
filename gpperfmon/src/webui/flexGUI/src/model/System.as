package model
{
	import mx.collections.ArrayCollection;
	import mx.collections.Sort;
	import mx.collections.SortField;
	import mx.collections.XMLListCollection;
	
	import util.Debug;
	import util.MiscUtils;
	
	import value.SystemSample;
		
	[Bindable]
	public class System
	{
		public function System(_mdl:GPPerfMonModelLocator):void
		{
			mdl = _mdl;	
			
			// Setup the sort on XMLList
			var s:Sort = new Sort();
			s.fields = [new SortField("@name", true, true)];
			systemNowXML.sort = s;
		}
		
		public function systemNow(systemData:XML):void
		{
			if (systemData.hasOwnProperty("@noresult")) {
				// we didnt get any response this time around. simply return!
				return;
			}
					
			// Assign the XML to a XMLListCollection for display
			systemNowXML.source = systemData.host;
			
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
			
			// Set the latest time based on the sample time
			mdl.session.setTimeVals(newVals.sample_time);
			
			systemNowAgg = newVals;		
			
			populateMinuteRollup();	
			
			populateLongerRollups();
		}

		//public function setXML

		// Rolls up the latest samples into 1 minute buckets, and appends each 
		// completed minute to the oneMinAggData array	
		private function populateMinuteRollup():void
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
				if (!oneMin_lastCalcDate || 
					(oneMin_lastCalcDate.time < systemNowCurrMinute.sample_time.time))
				{
					if (oneMin_lastCalcDate)
					{
						// Insert a gap object if the new datapoint isn't within 5 mins of the last
						var nextDate:Number = oneMin_lastCalcDate.time + (MiscUtils.MINUTE * 5);
						if (nextDate < systemNowCurrMinute.sample_time.time) {
							var missingData:Object = new Object;
							missingData.sample_time = new Date(nextDate);
							oneMinAggData.addItem(missingData);
						}
					}			
					oneMin_lastCalcDate = systemNowCurrMinute.sample_time;
					// Check if it changes the max values
					oneMinAggMax.calcMax(systemNowCurrMinute);
					// Insert it into the adday
					oneMinAggData.addItem(systemNowCurrMinute);
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

		// Updates the 5min, 1hr and 1day rollups based on the gathered realtime data	
		private function populateLongerRollups():void
		{
			if (!systemNowAgg || systemNowAgg.sample_time == null)
			{
				Debug.debug("systemNow: populateLongerRollup received null value");
				return;
			}

			// Do we need to do the 5 min rollup now?
			if (fiveMin_lastCalcDate)
			{				
				var prevFiveMins:Number = 
					MiscUtils.date_trim_5min(systemNowAgg.sample_time).time - (MiscUtils.MINUTE*5);
							
				if (fiveMin_lastCalcDate.time < prevFiveMins)
				{
					Debug.debug("systemNow: generating 5 min rollup for " + (new Date(prevFiveMins)).toString());
					populateFiveMinuteRollup(prevFiveMins);
				}
			}

			// Do we need to do the 1hr rollup now?
			if (oneHr_lastCalcDate)
			{				
				var prevOneHr:Number = 
					MiscUtils.date_trim_1hr(systemNowAgg.sample_time).time - (MiscUtils.HOUR);
							
				if (oneHr_lastCalcDate.time < prevOneHr)
				{
					Debug.debug("systemNow: generating 1hr rollup for " + (new Date(prevOneHr)).toString());
					populateOneHrRollup(prevOneHr);
				}
			}
			
			// Do we need to do the 1day rollup now?
			if (oneDay_lastCalcDate)
			{				
				var prevOneDay:Number = 
					MiscUtils.date_trim_1day(systemNowAgg.sample_time).time - (MiscUtils.DAY);
							
				if (oneDay_lastCalcDate.time < prevOneDay)
				{
					Debug.debug("systemNow: generating 1day rollup for " + (new Date(prevOneDay)).toString());
					populateOneDayRollup(prevOneDay);
				}
			}
		}

		// Uses 1min samples to build the next 5minute sample
		private function populateFiveMinuteRollup(sampleTime:Number):void
		{
			var newVal:SystemSample = new SystemSample;
			newVal.sample_time = new Date(sampleTime);

			var count:int = 0;
			for each(var _val:Object in oneMinAggData)
			{	
				var val:SystemSample = _val as SystemSample;
				if (val && (val.sample_time.time >= sampleTime &&
				    	    (val.sample_time.time < sampleTime+(MiscUtils.MINUTE*5))))
				{
					newVal.add(val);
					count++;
				}
			}
			
			Debug.debug("populateFiveMinuteRollup: found " + count.toString() +
			            " 1min samples");
			if (count > 0)
			{
				newVal.divideBy(count);
				
				if (fiveMin_lastCalcDate)
				{
					// Insert a gap object if there's a gap in the data
					var nextDate:Number = fiveMin_lastCalcDate.time + (MiscUtils.MINUTE*5);
					if (nextDate < sampleTime) {
						var missingData:Object = new Object;
						missingData.sample_time = new Date(nextDate);
						fiveMinAggData.addItem(missingData);
						Debug.debug("Inserting 5min sample gap at " + missingData.sample_time.toString());
					}
				}			
				fiveMin_lastCalcDate = newVal.sample_time;
				Debug.debug("Adding 5min sample for " + newVal.sample_time.toString());
				fiveMinAggMax.calcMax(newVal);
				fiveMinAggData.addItem(newVal);			
			}	          
		}

		// Uses 5min samples to build the next hour sample
		private function populateOneHrRollup(sampleTime:Number):void
		{
			var newVal:SystemSample = new SystemSample;
			newVal.sample_time = new Date(sampleTime);

			var count:int = 0;
			for each(var _val:Object in fiveMinAggData)
			{	
				var val:SystemSample = _val as SystemSample;
				if (val && (val.sample_time.time >= sampleTime &&
				    	    (val.sample_time.time < sampleTime+MiscUtils.HOUR)))
				{
					newVal.add(val);
					count++;
				}
			}
			
			Debug.debug("populateOneHourRollup: found " + count.toString() +
			            " 5min samples");
			if (count > 0)
			{
				newVal.divideBy(count);
				
				if (oneHr_lastCalcDate)
				{
					// Insert a gap object if there's a gap in the data
					var nextDate:Number = oneHr_lastCalcDate.time + (MiscUtils.HOUR);
					if (nextDate < sampleTime) {
						var missingData:Object = new Object;
						missingData.sample_time = new Date(nextDate);
						oneHrAggData.addItem(missingData);
						Debug.debug("Inserting 1hr sample gap at " + missingData.sample_time.toString());
					}
				}			
				oneHr_lastCalcDate = newVal.sample_time;
				Debug.debug("Adding 1hr sample for " + newVal.sample_time.toString());
				oneHrAggMax.calcMax(newVal);
				oneHrAggData.addItem(newVal);			
			}	          
		}

		// Uses 1hr samples to build the next 1day sample
		private function populateOneDayRollup(sampleTime:Number):void
		{
			var newVal:SystemSample = new SystemSample;
			newVal.sample_time = new Date(sampleTime);

			var count:int = 0;
			for each(var _val:Object in oneHrAggData)
			{	
				var val:SystemSample = _val as SystemSample;
				if (val && (val.sample_time.time >= sampleTime &&
				    	    (val.sample_time.time < sampleTime+MiscUtils.DAY)))
				{
					newVal.add(val);
					count++;
				}
			}
			
			Debug.debug("populateOneDayRollup: found " + count.toString() +
			            " 1hr samples");
			if (count > 0)
			{
				newVal.divideBy(count);
				
				if (oneDay_lastCalcDate)
				{
					// Insert a gap object if there's a gap in the data
					var nextDate:Number = oneDay_lastCalcDate.time + (MiscUtils.DAY);
					if (nextDate < sampleTime) {
						var missingData:Object = new Object;
						missingData.sample_time = new Date(nextDate);
						oneDayAggData.addItem(missingData);
						Debug.debug("Inserting 1day sample gap at " + missingData.sample_time.toString());
					}
				}			
				oneDay_lastCalcDate = newVal.sample_time;
				Debug.debug("Adding 1day sample for " + newVal.sample_time.toString());
				oneDayAggMax.calcMax(newVal);
				oneDayAggData.addItem(newVal);			
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
			
			if (numColumns == 0)
				Debug.debug("systemLoadHistoric found no values");
				
			// Keep track of dates to make sure each is greater than the last
			var latestDate:Date = null;			
			
			for (var j:int=0; j<maxLength; j++)
			{
				// object to store system metrics for a single slice in time
				var obj:SystemSample = new SystemSample();
				var thisDate:Date = null;
				var nextDate:Number;
				var missingData:Object;
				
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
					if (detail == '1min')
					{
						if (latestDate)
						{
							// // Insert a gap object if the new datapoint isn't within 5 mins of the last
							nextDate = latestDate.time + (MiscUtils.MINUTE*5);
							if (nextDate < thisDate.time) {
								missingData = new Object;
								missingData.sample_time = new Date(nextDate);
								oneMinAggData.addItem(missingData);
							}
						}
						oneMinAggMax.calcMax(obj);
						oneMinAggData.addItem(obj);
					}
					else if (detail == '5min')
					{
						if (latestDate)
						{
							// Insert a gap object if there's a gap in the data
							nextDate = latestDate.time + (MiscUtils.MINUTE*5);
							if (nextDate < thisDate.time) {
								missingData = new Object;
								missingData.sample_time = new Date(nextDate);
								fiveMinAggData.addItem(missingData);
							}
						}						
						fiveMinAggMax.calcMax(obj);
						fiveMinAggData.addItem(obj);
					}
					else if (detail == '1hr')
					{
						if (latestDate)
						{
							// Insert a gap object if there's a gap in the data
							nextDate = latestDate.time + MiscUtils.HOUR;
							if (nextDate < thisDate.time) {
								missingData = new Object;
								missingData.sample_time = new Date(nextDate);
								oneHrAggData.addItem(missingData);
							}
						}	
						oneHrAggMax.calcMax(obj);
						oneHrAggData.addItem(obj);
					}
					else if (detail == '1day')
					{
						if (latestDate)
						{
							// Insert a gap object if there's a gap in the data
							nextDate = latestDate.time + MiscUtils.DAY;
							if (nextDate < thisDate.time) {
								missingData = new Object;
								missingData.sample_time = new Date(nextDate);
								oneDayAggData.addItem(missingData);
							}
						}	
						oneDayAggMax.calcMax(obj);
						oneDayAggData.addItem(obj);	
					}
									
					latestDate = thisDate;
				}
				else
				{
					Debug.debug("System dropping historic data for date " + thisDate.toString());
				}
			}
			if (detail == '1min')
			{
				oneMin_firstReceivedDate = start;
				oneMin_lastReceivedDate = latestDate;
				oneMin_lastCalcDate = latestDate;
				dataLoaded[0] = true;
				dataLoaded[1] = true;
			}
			else if (detail == '5min')
			{
				fiveMin_firstReceivedDate = start;
				fiveMin_lastReceivedDate = latestDate;
				fiveMin_lastCalcDate = latestDate;				
				dataLoaded[2] = true;
			}
			else if (detail == '1hr')
			{
				oneHr_firstReceivedDate = start;
				oneHr_lastReceivedDate = latestDate;
				oneHr_lastCalcDate = latestDate;
				dataLoaded[3] = true;
				dataLoaded[4] = true;
			}
			else if (detail == '1day')
			{
				oneDay_firstReceivedDate = start;
				oneDay_lastReceivedDate = latestDate;
				oneDay_lastCalcDate = latestDate;		
				dataLoaded[5] = true;
				dataLoaded[6] = true;	
			}	
		}
		
		// Loads detailed data for a single point in time (based on selection)
		public function systemHistoricDetailed(detail:String, start:Date, end:Date, systemData:XML):void
		{
			if (!mdl.nav.selectedSystemItem || (start != mdl.nav.selectedSystemItem.sample_time))
			{
				Debug.debug("systemHistoricDetailed received sample for old selection - discarding");
				return;
			}

			selectedDataDetails = systemData;	
		}


		// Stores the most recent system now info, aggregated across systems
		public var systemNowAgg:SystemSample = null;
		// Stores the raw XML of the most recent system now response
		public var systemNowXML:XMLListCollection = new XMLListCollection();
		
		// Holds detailed date for a selected point in tine
		public var selectedDataDetails:XML = null;
		
		// Stores a running sum of the current minute's system information
		public var systemNowCurrMinute:SystemSample = null;
		// How many samples have gone into the current minute?
		public var systemNowCurrMinute_numSamples:int = 0;
		public var systemNow_lastReceivedDate:Date = null;
		
		// Stores last 6 hours at 1 minute granularity
		public var oneMinAggData:ArrayCollection = new ArrayCollection;
		// Stores the max values for graphing
		public var oneMinAggMax:SystemSample = new SystemSample;
		// Last date for which we've received data
		public var oneMin_firstReceivedDate:Date = null;
		public var oneMin_lastReceivedDate:Date = null;
		public var oneMin_lastCalcDate:Date = null;
		
		// Stores last 24 hours at 5 minute granularity
		public var fiveMinAggData:ArrayCollection = new ArrayCollection;
		// Stores the max values for graphing
		public var fiveMinAggMax:SystemSample = new SystemSample;
		public var fiveMin_firstReceivedDate:Date = null;
		public var fiveMin_lastReceivedDate:Date = null;
		public var fiveMin_lastCalcDate:Date = null;
		
		// Stores last 30 days at 1 hr granularity
		public var oneHrAggData:ArrayCollection = new ArrayCollection;	
		// Stores the max values for graphing
		public var oneHrAggMax:SystemSample = new SystemSample;
		public var oneHr_firstReceivedDate:Date = null;
		public var oneHr_lastReceivedDate:Date = null;
		public var oneHr_lastCalcDate:Date = null;
		
		// Stores last 2 years at 1 day granularity 
		public var oneDayAggData:ArrayCollection = new ArrayCollection;			
		// Stores the max values for graphing
		public var oneDayAggMax:SystemSample = new SystemSample;		
		public var oneDay_firstReceivedDate:Date = null;
		public var oneDay_lastReceivedDate:Date = null;		
		public var oneDay_lastCalcDate:Date = null;
		
		// Lookup to find the appropriate data provider at each duration
		public var activeAggLookup:ArrayCollection = new ArrayCollection([
			oneMinAggData, oneMinAggData, fiveMinAggData, oneHrAggData, 
			oneHrAggData, oneDayAggData, oneDayAggData]);	

		// Lookup to find the appropriate max at each duration
		public var activeMaxLookup:Array = new Array(
			oneMinAggMax, oneMinAggMax, fiveMinAggMax, oneHrAggMax, 
			oneHrAggMax, oneDayAggMax, oneDayAggMax);

		// Is the data loaded for each duration
		public var dataLoaded:ArrayCollection = new ArrayCollection( 
			new Array(false, false, false, false, 
					  false, false, false));

		// Lookup to find interval for each val
		public var dataIntervalNumber:ArrayCollection = 
			new ArrayCollection([1, 1, 5, 1, 1, 1, 1]);
		public var dataIntervalUnit:ArrayCollection =
			new ArrayCollection(['minutes', 'minutes', 'minutes', 'hours', 'hours', 'days', 'days']);

		// Reference to the model
		private var mdl:GPPerfMonModelLocator;	
	}
}
