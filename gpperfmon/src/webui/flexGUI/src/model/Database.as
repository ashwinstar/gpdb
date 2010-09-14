package model
{
	import mx.collections.ArrayCollection;
	
	import util.Debug;
	import util.MiscUtils;
	
	import value.DatabaseSample;
		
	[Bindable]
	public class Database
	{
		public function Database(_mdl:GPPerfMonModelLocator):void
		{
			mdl = _mdl;	
		}
		
		public function databaseNow(db:XML):void
		{			
			var newVal:DatabaseSample = new DatabaseSample;
			
			if (db.hasOwnProperty("@dbstatus")) {
				databaseStatus(db.@dbstatus);	
			}
			
			if (db.hasOwnProperty("@noresult")) {
				// we didnt get any results! -- this probably means the db is down!
				return;
			}
					
			if (newVal.sample_time == null)
			{
				newVal.sample_time = MiscUtils.parseDate(db.sample_time);
				if (databaseNow_lastReceivedDate && 
				    databaseNow_lastReceivedDate.time >= newVal.sample_time.time)
				{
					return;
				}
				else
					databaseNow_lastReceivedDate = newVal.sample_time;
			}

			newVal.queries_running = db.queries_running;
			newVal.queries_running_max = db.queries_running;
			newVal.queries_running_min = db.queries_running;
			newVal.queries_queued = db.queries_queued;
			newVal.queries_queued_max = db.queries_queued;
			newVal.queries_queued_min = db.queries_queued;
						
			databaseNowAgg = newVal;		
			
			populateMinuteRollup();	
			
			populateLongerRollups();
		}

		// Rolls up the latest samples into 1 minute buckets, and appends each 
		// completed minute to the oneMinAggData array	
		private function populateMinuteRollup():void
		{
			if (!databaseNowAgg || databaseNowAgg.sample_time == null)
			{
				Debug.debug("databaseNow: populateMinuteRollup received null value");
				return;
			}
				
			var sampleMinute:Date = MiscUtils.date_trim_1min(databaseNowAgg.sample_time);
			
			if (!databaseNowCurrMinute || databaseNowCurrMinute_numSamples == 0)
			{
				// Start the per-minute rollup
				
				databaseNowCurrMinute = databaseNowAgg;
				databaseNowCurrMinute.sample_time = sampleMinute;
				databaseNowCurrMinute_numSamples = 1;
			}
			else if (databaseNowCurrMinute.sample_time.time != sampleMinute.time)
			{
				// Finish last per-minute sample and start a new one
				
				databaseNowCurrMinute.divideBy(databaseNowCurrMinute_numSamples);
		
				// Add this to the array
				if (!oneMin_lastCalcDate || 
					(oneMin_lastCalcDate.time < databaseNowCurrMinute.sample_time.time))
				{
					if (oneMin_lastCalcDate)
					{
						// Insert a gap object if there's a gap in the data
						var nextDate:Number = oneMin_lastCalcDate.time + MiscUtils.MINUTE;
						if (nextDate < databaseNowCurrMinute.sample_time.time) {
							var missingData:Object = new Object;
							missingData.sample_time = new Date(nextDate);
							oneMinAggData.addItem(missingData);
						}
					}			
					oneMin_lastCalcDate = databaseNowCurrMinute.sample_time;
					oneMinAggMax.calcMax(databaseNowCurrMinute);
					oneMinAggData.addItem(databaseNowCurrMinute);
				}	
				
				// Restart the per-minute rollup
				databaseNowCurrMinute = databaseNowAgg;
				databaseNowCurrMinute.sample_time = sampleMinute;
				databaseNowCurrMinute_numSamples = 1;
			}
			else
			{
				// Add to the existing rollup			
				databaseNowCurrMinute.add(databaseNowAgg);		
				
				databaseNowCurrMinute_numSamples += 1;
			}
		}

		// Updates the 5min, 1hr and 1day rollups based on the gathered realtime data	
		private function populateLongerRollups():void
		{
			if (!databaseNowAgg || databaseNowAgg.sample_time == null)
			{
				Debug.debug("databaseNow: populateLongerRollup received null value");
				return;
			}

			// Do we need to do the 5 min rollup now?
			if (fiveMin_lastCalcDate)
			{				
				var prevFiveMins:Number = 
					MiscUtils.date_trim_5min(databaseNowAgg.sample_time).time - (MiscUtils.MINUTE*5);
							
				if (fiveMin_lastCalcDate.time < prevFiveMins)
				{
					Debug.debug("databaseNow: generating 5 min rollup for " + (new Date(prevFiveMins)).toString());
					populateFiveMinuteRollup(prevFiveMins);
				}
			}

			// Do we need to do the 1hr rollup now?
			if (oneHr_lastCalcDate)
			{				
				var prevOneHr:Number = 
					MiscUtils.date_trim_1hr(databaseNowAgg.sample_time).time - (MiscUtils.HOUR);
							
				if (oneHr_lastCalcDate.time < prevOneHr)
				{
					Debug.debug("databaseNow: generating 1hr rollup for " + (new Date(prevOneHr)).toString());
					populateOneHrRollup(prevOneHr);
				}
			}
			
			// Do we need to do the 1day rollup now?
			if (oneDay_lastCalcDate)
			{				
				var prevOneDay:Number = 
					MiscUtils.date_trim_1day(databaseNowAgg.sample_time).time - (MiscUtils.DAY);
							
				if (oneDay_lastCalcDate.time < prevOneDay)
				{
					Debug.debug("databaseNow: generating 1day rollup for " + (new Date(prevOneDay)).toString());
					populateOneDayRollup(prevOneDay);
				}
			}
		}

		// Uses 1min samples to build the next 5minute sample
		private function populateFiveMinuteRollup(sampleTime:Number):void
		{
			var newVal:DatabaseSample = new DatabaseSample;
			newVal.sample_time = new Date(sampleTime);

			var count:int = 0;
			for each(var _val:Object in oneMinAggData)
			{	
				var val:DatabaseSample = _val as DatabaseSample;
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
			var newVal:DatabaseSample = new DatabaseSample;
			newVal.sample_time = new Date(sampleTime);

			var count:int = 0;
			for each(var _val:Object in fiveMinAggData)
			{	
				var val:DatabaseSample = _val as DatabaseSample;
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
			var newVal:DatabaseSample = new DatabaseSample;
			newVal.sample_time = new Date(sampleTime);

			var count:int = 0;
			for each(var _val:Object in oneHrAggData)
			{	
				var val:DatabaseSample = _val as DatabaseSample;
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


		// Load historic database data
		public function databaseHistoric(detail:String, start:Date, end:Date, databaseData:XML):void
		{
			var elementArray:Array = new Array();
			var nameArray:Array = new Array();
			
			var maxLength:int = 0;
			var numColumns:int = 0;
			
			if (databaseData.hasOwnProperty("@dbstatus"))
			{
				databaseStatus(databaseData.@dbstatus);			
			}

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
				Debug.debug("Call to databaseLoadHistoric with bad interval: " + detail);
				return;
			}
			
			// iterate through all elements in the parent hostname node.
			for each (var column:XML in databaseData.* )
			{
				var valueArray:Array = column.toString().split('|');
				
				if (valueArray.length > maxLength)
				{
					maxLength = valueArray.length;
				}
				elementArray.push(valueArray);
				var colName:String = column.name();
				nameArray.push(column.name());
			}
			numColumns = nameArray.length;
			
			if (numColumns == 0)
				Debug.debug("databaseLoadHistoric found no values");

			// Keep track of dates to make sure each is greater than the last
			var latestDate:Date = null;			
			
			for (var j:int=0; j<maxLength; j++)
			{
				// object to store database metrics for a single slice in time
				var obj:DatabaseSample = new DatabaseSample();
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
					else if (String(nameArray[i]).substr(0,13) != 'queries_total')
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
							// Insert a gap object if there's a gap in the data
							nextDate = latestDate.time + MiscUtils.MINUTE;
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
					Debug.debug("Database dropping historic data for date " + thisDate.toString());
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
		
		/**
		 * This function shows the database status on the main view.
		 * It changes the icon and lable as per the result from the data base.
		 * For NORMAL state nothing will be shown.
		 * For DEGRADE and POTENTIALLY UNBALANCED states corresponding icons will be shown. 
		 * @param dbState
		 */		
		public function databaseStatus(_dbState:String):void
		{
			dbState = _dbState;
			
			if (dbState == DBSTATE_DEGRADED) {
				dbStatus = "Segment Failure(s)";
				dbStatusToolTip = "One or more segment failures in the system.";
			} else if (dbState == DBSTATE_UNBALANCED) {
				dbStatus = "Suboptimal Segment Configuration";
				dbStatusToolTip = "One or more segments is not operating in its preferred role for the system";
			} else if (dbState == DBSTATE_DOWN) {
				dbStatus = "Database Unreachable";
				dbStatusToolTip = "Unable to get a response from the Database";
			} else {
				dbStatus = "Normal";
				dbStatusToolTip = "Normal Operations";
			}
		}//end of databaseStatus


		// Stores the most recent database now info, aggregated across databases
		public var databaseNowAgg:DatabaseSample = null;
		// Stores the raw XML of the most recent database now response
		public var databaseNowXML:XML = null;
		
		// Stores a running sum of the current minute's database information
		public var databaseNowCurrMinute:DatabaseSample = null;
		// How many samples have gone into the current minute?
		public var databaseNowCurrMinute_numSamples:int = 0;
		public var databaseNow_lastReceivedDate:Date = null;
		
		// Stores last 6 hours at 1 minute granularity
		public var oneMinAggData:ArrayCollection = new ArrayCollection;
		// Stores the max values for graphing
		public var oneMinAggMax:DatabaseSample = new DatabaseSample;
		// Last date for which we've received data
		public var oneMin_firstReceivedDate:Date = null;
		public var oneMin_lastReceivedDate:Date = null;
		public var oneMin_lastCalcDate:Date = null;
		
		// Stores last 24 hours at 5 minute granularity
		public var fiveMinAggData:ArrayCollection = new ArrayCollection;
		// Stores the max values for graphing
		public var fiveMinAggMax:DatabaseSample = new DatabaseSample;
		public var fiveMin_firstReceivedDate:Date = null;
		public var fiveMin_lastReceivedDate:Date = null;
		public var fiveMin_lastCalcDate:Date = null;
		
		// Stores last 30 days at 1 hr granularity
		public var oneHrAggData:ArrayCollection = new ArrayCollection;	
		// Stores the max values for graphing
		public var oneHrAggMax:DatabaseSample = new DatabaseSample;
		public var oneHr_firstReceivedDate:Date = null;
		public var oneHr_lastReceivedDate:Date = null;
		public var oneHr_lastCalcDate:Date = null;
		
		// Stores last 2 years at 1 day granularity 
		public var oneDayAggData:ArrayCollection = new ArrayCollection;			
		// Stores the max values for graphing
		public var oneDayAggMax:DatabaseSample = new DatabaseSample;		
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
		
		// Reference to the model
		private var mdl:GPPerfMonModelLocator;
		
		//Database Status
		public static var DBSTATE_NORMAL:String = "NORMAL";
		public static var DBSTATE_UNBALANCED:String = "UNBALANCED";
		public static var DBSTATE_DEGRADED:String = "DEGRADED";
		public static var DBSTATE_DOWN:String = "DOWN";
		
		public var dbState:String = "";
		public var dbStatus:String = "";
		public var dbStatusToolTip:String = "";
	}
}
