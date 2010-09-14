package util {
	import mx.collections.ArrayCollection;
	import mx.controls.Alert;
	import mx.controls.DataGrid;
	import mx.controls.dataGridClasses.DataGridColumn;
	import mx.rpc.events.FaultEvent;
	import mx.rpc.events.ResultEvent;
	import mx.collections.Sort;
	import mx.collections.SortField;
	
	public class MiscUtils 
	{
		public static const MILLISECOND:Number = 1;
		public static const SECOND:Number = MILLISECOND * 1000;
		public static const MINUTE:Number = SECOND * 60;
		public static const HOUR:Number = MINUTE * 60;
		public static const DAY:Number = HOUR * 24;
		public static const WEEK:Number = DAY * 7;
		public static const MONTH:Number = DAY * 30;
		public static const YEAR:Number = DAY * 365;
		
		public static function checkHttpServiceResultsForFailure(caller:String, event:ResultEvent, arrayIsNull:Boolean, suppressPopUp:Boolean = false):Boolean
		{
			if(event.result.error != null)
			{
				if(! suppressPopUp)
				{
					if(event.result.error.message)
						Alert.show(caller + " " + event.result.error.message);
					else
						Alert.show(caller + " HTTPService returned a generic error");
				}
		
				return true;
			}
			else if(arrayIsNull)
			{
				if(! suppressPopUp)
					Alert.show(caller + " HTTPService failed to produce expected results");
		
				return true;
			}
		
			return false;
		}
		
		public static function checkHttpServiceResultsForFailureWithoutNullityCheck(caller:String, event:ResultEvent):Boolean
		{
			if(event.result.error != null)
			{
				if(event.result.error.message)
					Alert.show(caller + " " + event.result.error.message);
				else
					Alert.show(caller + " HTTPService returned a generic error");
				return true;
			}
		
			return false;
		}
		
		public static function faultHandler(event:FaultEvent,name:String):void
		{
			Alert.show(name.concat(" found fault: ",event.fault.faultString));
		}
		
		public static function selectFromDropDown(provider:Object, item:String):int
		{
			for(var i:Number=0; i<provider.length; i++)
			{
				if(provider[i].data == item)
				{
					return i;
				}
			}
			
			return -1;
		}
		
		public static function normalize(value:String, total:String):int
		{	
			var numTotal:int = parseFloat(total);
			if(numTotal != 0)
			{
				return (parseFloat(value)/numTotal)*100;
			}
			// invalid data
			return 0;
		}
		
		public static function parseBoolean(input:String):Boolean
		{
			if(input == 'T' || input == 't' || input == 'true' || input == 'True' || input == 'TRUE' || input == '1')
				return true;
			else
				return false;
		}
		
		public static function parseDate(input:String):Date
		{
			if(!input || input == '' || input == 'null')
				return null;
		
			var pattern:RegExp = /-/g;
			input = input.replace(pattern, '/');
		
			var date:Date = new Date();
			date.setTime(Date.parse(input));
		
			return date;
		}
		
		public static function formatDateToStringUTC(input:Number):String
		{
			if (input >= 0)
			{
				var foo:Date = new Date(input);
			
				// 2008-08-28 15:20:00
				return foo.getUTCFullYear() + "-" + padInteger(foo.getUTCMonth() + 1) + "-" + padInteger(foo.getUTCDate()) + " " + 
					padInteger(foo.getUTCHours()) + ":" + padInteger(foo.getUTCMinutes()) + ":" + padInteger(foo.getUTCSeconds());
			} 
			else
				return "";
		}
		
		public static function formatDateToString(input:Number):String
		{
			if (input >= 0)
			{
				var foo:Date = new Date(input);
			
				// 2008-08-28 15:20:00
				return foo.getFullYear() + "-" + padInteger(foo.getMonth() + 1) + "-" + padInteger(foo.getDate()) + " " + 
					padInteger(foo.getHours()) + ":" + padInteger(foo.getMinutes()) + ":" + padInteger(foo.getSeconds());
			} 
			else
				return "";
		}
		
		private static function padInteger(input:int):String
		{
			if(input < 10)
				return "0" + input;
			else
				return "" + input;
		}
		
		// Returns date rounded off to the minute
		public static function date_trim_1min(input:Date):Date
		{
			return new Date(input.fullYear, input.month, input.date, input.hours, input.minutes);
		}

		// Returns date rounded off to 5 minutes
		public static function date_trim_5min(input:Date):Date
		{
			return new Date(input.fullYear, input.month, input.date, input.hours, 5 * int(input.minutes/5));
		}

		// Returns date rounded off to 1 hr
		public static function date_trim_1hr(input:Date):Date
		{
			return new Date(input.fullYear, input.month, input.date, input.hours);
		}

		// Returns date rounded off to 1 day
		public static function date_trim_1day(input:Date):Date
		{
			return new Date(input.fullYear, input.month, input.date);
		}


		// Returns number of previous minute rounded off to the minute
		public static function prevMinAsNumber(date:Date):Number
		{
			var tmpDate:Date = new Date(date.fullYear, date.month, date.date, date.hours, date.minutes);
			return (tmpDate.time - MINUTE);
		}
				
		public static function serializeDataGrid( grid:DataGrid, fields:Array ):String 
		{
			var rows:ArrayCollection;
			var output:String;
			var i:int;
			var j:int;
			
			output = "";
			rows = grid.dataProvider as ArrayCollection;
			
			if(! rows )
				return output;
			
			for( i=0; i<rows.length; i++ ) 
			{
				for( j=0; j<fields.length; j++ ) 
				{
					output += rows[i][fields[j]] + "~";
				}
				output += ",";
			}
			
			return output;	
		}
	
		// Removes line feeds from the text
		public static function noLineFeed ( text:String ):String
		{
			var res:String = '';
			
			for (var i:int = 0; i<text.length; i++)
		    {
		    	if (text.charAt(i) != '\r')
		    		res += text.charAt(i);
		    }	
		    return res;
		}	


		public static function sortDataGrid(dgGrid:DataGrid, dataField:String, descendingFlag:Boolean):void {
			if (dataField != null && dataField != "") {
				var sortThese:Sort = new Sort();
				sortThese.fields = [new SortField(dataField, false, descendingFlag)];
				dgGrid.dataProvider.sort = sortThese;
				dgGrid.dataProvider.refresh();
			}
		}

	}
}
