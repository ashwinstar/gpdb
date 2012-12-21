package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.ServiceUtils;
	import mx.rpc.IResponder;
				
	public class SystemNowCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	
		
        public function SystemNowCommand() {     
        }
		
		public function execute():void
		{
			ServiceUtils.send(mdl.session.webServiceURL+'system', this);
		}
        							    
		// Called when the SystemNow request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'system')
				mdl.system.systemNow(result);
			else if (result.name() == 'error' && result.code == 'ACCESS_DENIED')
			{
				Debug.debug("SystemNow error: ACCESS DENIED");
				mdl.session.accessDenied();
			}	
			else
				Debug.debug("SystemNow error: " + result);
			
		 	executeChainedCommand(); 
		}
		
		// Called when the SystemNow request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("SystemNow fault: " + faultEvent.toString());
			mdl.session.connectionBad();
			
			executeChainedCommand(); 
		}
	}
}


