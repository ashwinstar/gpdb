package command
{
	import model.GPPerfMonModelLocator;
	
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	
	import util.Debug;
	import util.ServiceUtils;
				
	public class HealthDetailsCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();
		
		private var hostName:String;
		private var callBack:Function;
		
        public function HealthDetailsCommand() {     
        }
		
		public function setParameters(host:String, callBackFunc:Function=null):void
		{
			hostName = host ;
			callBack = callBackFunc;
		}
		
		public function execute():void
		{
			var req:String = '?hostname=' + hostName;
			ServiceUtils.send(mdl.session.webServiceURL+'healthdetails'+req, this);
		}
        							    
		// Called when the Health request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'HealthDetails') {
				//mdl.health.healthDetails(result);
			} else if (result.name() == 'error' && result.code == 'ACCESS_DENIED') {
				Debug.debug("Health error: ACCESS DENIED");
				mdl.session.accessDenied();
			} else {
				Debug.debug("Health error: " + result);
			}
			
		 	executeChainedCommand();
		 	
		 	if (result.name() == 'HealthDetails' && callBack != null) {
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


