package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	import util.ServiceUtils;
	import util.Debug;
		
	public class LogoffCommand implements IResponder
	{	
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();
						
        public function LogoffCommand() {     
        }
		
		public function execute():void
		{
			ServiceUtils.send(mdl.session.databaseURL+'logoff', this);
		}
		
		public function result(data:Object):void
		{
        	var logoff:XML = XML(data.result);
        	
        	var status:String = logoff.status;
        	Debug.debug("Logoff as " + logoff.username + ": " + status);
		}
		
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
            Debug.debug("Logoff: unable to connect to the server");
		}
	}
}


