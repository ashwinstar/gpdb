package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	import mx.controls.Alert;
	import util.Debug;
	import util.ServiceUtils;
	
	public class ConfigCommand implements IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	
						
		// Callback function to return the status of the operation
		private var configCallback:Function = null;
		
        public function ConfigCommand() {     
        }
		
		public function execute(callback:Function=null):void
		{
			configCallback = callback;
			ServiceUtils.send(mdl.session.databaseURL+'config', this);
		}
		
		public function result(data:Object):void
		{
			var result:XML = XML(data.result);
			
			if (result.name() != 'config')
			{
				Debug.debug("Config error: " + result);
				configCallback(false, "Received error response from server");
			}	
			
        	mdl.session.config = result;
        	Debug.debug("Config: request succeeded");
        	// Make sure we can read the required values
        	mdl.session.monitorQuantum = int(mdl.session.config.GPMMON.quantum);
        	if (mdl.session.monitorQuantum <= 0 || 
        	    mdl.session.monitorQuantum > 60)
        	{
        		Debug.debug("Config: unable to read required GPMMON:quantum");
        		if (configCallback != null)
        			configCallback(false, "Unable to access required config settings");	
        	}	
			else
				configCallback(true);        	
		}
		
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("Config: request failed");
            if (configCallback != null)
            	configCallback(false, "Unable to read config from server");
		}
	}
}


