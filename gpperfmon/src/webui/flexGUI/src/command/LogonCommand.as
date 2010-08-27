package command
{
	import model.GPPerfMonModelLocator;
	
	import mx.controls.Alert;
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	
	import util.Debug;
	import util.ServiceUtils;
				
	public class LogonCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	
						
		// Internal temporary storage
		private var username:String;
		private var role:String;
		private var appliance:Boolean = false ;
						
		// These hold the parameters
		private var login:String;
		private var password:String;
						
		// Callback function to return the status of the login
		private var loginCallback:Function = null;
		
        public function LogonCommand() {     
        }
        
        public function setParameters(_login: String, _password:String, callback:Function=null):void
        {
			loginCallback = callback;     
			login = _login;
			password = _password;
        }
		
		public function execute():void
		{
			ServiceUtils.send(mdl.session.webServiceURL+'logon',
							  this,
							  {username: login, password: password});
			password = '';
		}
		
		// Result of follow-on call to get configuration information
		//  - result of the ConfigCommand called from the result function
		public function configResult(success:Boolean, message:String=''):void
		{
			if (success)
			{
				Debug.debug("Login: successfully got config info");
		    	if (loginCallback != null)
        			loginCallback(true, 'Login succeeded', 
        								username, role, appliance);				
			}
			else
			{
				Debug.debug("Login: failed to get config info");
        		if (loginCallback != null)
        			loginCallback(false, message);				
			}
		}

        							    
		// Called when the logon request returns successfully
		public function result(data:Object):void
		{
        	var logon:XML = XML(data.result);
        	
 			if (logon.name() == 'logon' && logon.status == 'SUCCESS')
        	{
        		Debug.debug("Login as " + logon.username + ": SUCCESS");
        		
        		// Set the session ID
        		mdl.session.sessionid = logon.sessionid;
        		username = logon.username;
        		role = logon.role;
        		
        		if (logon.hasOwnProperty("Device")) {
        			appliance = (logon.Device == "Appliance");
        		}
        		
				// Now fetch config information
				var configCommand:ConfigCommand = new ConfigCommand;
				configCommand.setCallback(configResult);
				configCommand.execute();
				
        	} 
        	else if (logon.name() == 'error' && logon.code == 'ACCESS_DENIED')
  			{
  				Debug.debug("Login: " + logon.message);
          		if (loginCallback != null)
          			loginCallback(false, logon.message);      		
        	} 
        	else 
        	{
        		Debug.debug("Login: Error communicating with server");
        		if (loginCallback != null)
        			loginCallback(false, 'Error communicating with server');
        	}
        	
        	executeChainedCommand(); 
		}
		
		// Called when the logon request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("Login: Unable to connect to the server");
            if (loginCallback != null)
            	loginCallback(false, "Unable to connect to the server");
            	
            executeChainedCommand(); 
		}
	}
}


