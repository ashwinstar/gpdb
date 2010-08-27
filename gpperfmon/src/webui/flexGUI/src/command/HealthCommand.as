package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.ServiceUtils;
	import mx.rpc.IResponder;
  	import mx.controls.Alert;	
				
	public class HealthCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();
		
		private var callBack:Function = null;
		
        public function HealthCommand() {     
        }
        
        public function setParameters(callBackFunc:Function = null):void
        {
        	callBack = callBackFunc;
        }
		
		public function execute():void
		{
			ServiceUtils.send(mdl.session.webServiceURL+'health', this);
		}
        							    
		// Called when the Health request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'Health')
				mdl.health.setHealthSummary(result);
			else if (result.name() == 'error' && result.code == 'ACCESS_DENIED')
			{
				Debug.debug("Health error: ACCESS DENIED");
				mdl.session.accessDenied();
			}	
			else
				Debug.debug("Health error: " + result);
		
		 	executeChainedCommand();
		 	
		 	if (callBack != null) {
		 		callBack(result);
		 	}
		}
		
		// Called when the Health request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("Health fault: " + faultEvent.toString());
			mdl.session.connectionBad();
			
		 	executeChainedCommand(); 
		}
	}
}


