package command
{
	import model.GPPerfMonModelLocator;
	
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	
	import util.Debug;
	import util.MiscUtils;
	import util.ServiceUtils;
				
	public class QuerySearchCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	

		// Store parameters to the request
		private var _id:String = '';
		private var _start:Number = 0;
		private var _end:Number = 0;
		private var _status:String = '';
		private var _mintime:Number = 0;
		private var _username:String = '';
		private var _db:String = '';
		
        public function QuerySearchCommand() {     
        }

		public function setParameters(id:String, start:Number=-1, end:Number=-1, 
									status:String='', mintime:Number=0, 
									username:String='', db:String=''):void
		{
			if (id)
			  _id = id;
			else 
			{
			  _start = start;
			  _end = end;
			  _status = status;
			  _mintime = mintime;
			  _username = username;
			  _db = db;
			}	
		}
		
		public function execute():void
		{
			var prefix:String = '?';
			var req:String = '';
			
			if (_id)
			{
				req = "?id=" + _id + "&realtime=no";
				
				ServiceUtils.send(mdl.session.webServiceURL+'querydetails'+req, this);			
			} 
			else
			{
				if (_start >= 0)
				{
					req += (prefix + "start=" + MiscUtils.formatDateToString(_start));
					prefix = '&';	
				}
				if (_end >= 0)
				{
					req += (prefix + "end=" + MiscUtils.formatDateToString(_end));
					prefix = '&';	
				}			
				if (_status != '')
				{
					req += (prefix + "status=" + _status);
					prefix = '&';	
				}			
				if (_mintime > 0)
				{
					req += (prefix + "mintime=" + _mintime.toString());
					prefix = '&';				
				}	
				if (_username != '')
				{
					req += (prefix + "username=" + _username);
					prefix = '&';				
				}	
				if (_db != '')
				{
					req += (prefix + "db=" + _db);
					prefix = '&';				
				}	
				req += (prefix + "textlength=1024")
				
				ServiceUtils.send(mdl.session.webServiceURL+'queries'+req, this);
			}
		}
        							    
		// Called when the QuerySearch request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'queries')
				mdl.queries.queriesSearch(result);
			else if (result.name() == 'error' && result.code == 'ACCESS_DENIED')
			{
				Debug.debug("QuerySearch error: ACCESS DENIED");
				mdl.session.accessDenied();
			}	
			else
			{
				Debug.debug("QuerySearch error: " + result);
				mdl.queries.queriesSearchFailed(result.code, result.message);	
			}
			
	 		executeChainedCommand(); 
		}
		
		// Called when the QuerySearch request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("QuerySearch fault: " + faultEvent.toString());
			mdl.session.connectionBad();
			
		 	executeChainedCommand(); 
		}
	}
}


