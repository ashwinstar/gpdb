package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.ServiceUtils;
	import mx.rpc.IResponder;
				
	public class DatabaseNowCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	
		
        public function DatabaseNowCommand() {     
        }
		
		public function execute():void
		{
			ServiceUtils.send(mdl.session.webServiceURL+'database', this);
		}
        							    
		// Called when the DatabaseNow request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'database')
				mdl.db.databaseNow(result);
			else if (result.name() == 'error' && result.code == 'ACCESS_DENIED')
			{
				Debug.debug("DatabaseNow error: ACCESS DENIED");
				mdl.session.accessDenied();
			}	
			else
				Debug.debug("DatabaseNow error: " + result);
		
		 	executeChainedCommand(); 
		}
		
		// Called when the DatabaseNow request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("DatabaseNow fault: " + faultEvent.toString());
			mdl.session.connectionBad();
			
		 	executeChainedCommand(); 
		}
	}
}


