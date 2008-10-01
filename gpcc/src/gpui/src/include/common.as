// $Id: common.as,v 1.17 2007-08-11 07:21:26 jistok Exp $i

import mx.collections.ArrayCollection;
import mx.controls.Alert;
import mx.controls.DataGrid;
import mx.events.*;
import mx.rpc.events.FaultEvent;
import mx.rpc.events.ResultEvent;

public function getURL():String
{
	var host:String = mx.core.Application.application.parameters.serverHost;
	var port:String = mx.core.Application.application.parameters.serverPort;
	
	if(host == null)
		host = "localhost";
	if(port == null)
		port = "8000";
	
	if(port == "8000")
		return "https://" + host + ":" + port + "/";
	else
		return "http://" + host + ":" + port + "/";
}

public function checkHttpServiceResultsForFailure(caller:String, event:ResultEvent, arrayIsNull:Boolean, suppressPopUp:Boolean = false):Boolean
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

public function checkHttpServiceResultsForFailureWithoutNullityCheck(caller:String, event:ResultEvent):Boolean
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

public function faultHandler(event:FaultEvent,name:String):void
{
	Alert.show(name.concat(" found fault: ",event.fault.faultString));
}

public function selectFromDropDown(provider:Object, item:String):int
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

public function normalize(value:String, total:String):int
{	
	var numTotal:int = parseFloat(total);
	if(numTotal != 0)
	{
		return (parseFloat(value)/numTotal)*100;
	}
	// invalid data
	return 0;
}

public function parseBoolean(input:String):Boolean
{
	if(input == 'T' || input == 't' || input == 'true' || input == 'True' || input == 'TRUE' || input == '1')
		return true;
	else
		return false;
}

public function parseDate(input:String):Date
{
	if(input == null || input == '')
		return null;

	var pattern:RegExp = /-/g;
	input = input.replace(pattern, '/');

	var date:Date = new Date();
	date.setTime(Date.parse(input));

	return date;
}

public function formatDateFromEpoch(input:Number):String
{
	var foo:Date = new Date(input);
	
	// 2008-08-28 15:20:00
			
	return foo.getUTCFullYear() + "-" + padInteger(foo.getUTCMonth() + 1) + "-" + padInteger(foo.getUTCDate()) + " " + 
		padInteger(foo.getUTCHours()) + ":" + padInteger(foo.getUTCMinutes()) + ":" + padInteger(foo.getUTCSeconds());
}

public function padInteger(input:int):String
{
	if(input < 10)
		return "0" + input;
	else
		return "" + input;
}
public function serializeDataGrid( grid:DataGrid, fields:Array ):String 
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

public function fixComboBox( event:Event ):void 
{
	var elements:ArrayCollection;
	
	elements = event.currentTarget.dataProvider as ArrayCollection;
	if( elements.length > 0 ) 
		elements[0].label = " ";
}
