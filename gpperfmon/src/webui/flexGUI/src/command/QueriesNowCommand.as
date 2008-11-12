package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.ServiceUtils;
			
	public class QueriesNowCommand implements IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	
		
        public function QueriesNowCommand() {     
        }
		
		public function execute():void
		{
			ServiceUtils.send(mdl.session.databaseURL+'queries?textlength=1024', this);
		}
        							    
		// Called when the QueriesNow request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'queries')
				mdl.queries.queriesNow(result);
			else if (result.name() == 'error' && result.code == 'ACCESS_DENIED')
			{
				Debug.debug("QueriesNow error: ACCESS DENIED");
				mdl.session.accessDenied();
			}	
			else
				Debug.debug("QueriesNow error: " + result);
		}
		
		// Called when the QueriesNow request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("QueriesNow fault: " + faultEvent.toString());
			mdl.session.connectionBad();
		}
	}
}


