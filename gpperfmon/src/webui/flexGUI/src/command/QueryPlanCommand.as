package command
{
	import model.GPPerfMonModelLocator;
	
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	
	import util.Debug;
	import util.ServiceUtils;
				
	public class QueryPlanCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	

		// Store parameters to the request
		private var queryID:String = '';
		private var queryRealtime:Boolean = false;
		
        public function QueryPlanCommand() {     
        }

		public function setParameters(id:String, realtime:Boolean):void
		{
			queryID = id;
			queryRealtime = realtime;		
		}
		
		public function execute():void
		{
			var req:String = '?id=' + queryID;
			req += '&realtime=' + (queryRealtime?'yes':'no');
			
			ServiceUtils.send(mdl.session.webServiceURL+'queryplan'+req, this);
		}
        							    
		// Called when the QueryPlan request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'queryplan')
				mdl.queries.queryPlanResult(result);
			else if (result.name() == 'error' && result.code == 'ACCESS_DENIED')
			{
				Debug.debug("QueryPlan error: ACCESS DENIED");
				mdl.session.accessDenied();
			}	
			else
				Debug.debug("QueryPlan error: " + result);
				
	 		executeChainedCommand(); 
		}
		
		// Called when the QueryPlan request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("QueryPlan fault: " + faultEvent.toString());
			mdl.session.connectionBad();
			
		 	executeChainedCommand(); 
		}
	}
}


