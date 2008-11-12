package command
{
	import model.GPPerfMonModelLocator;
	
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.ServiceUtils;
	import command.ConfigCommand;
			
	public class LogonCommand implements IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	
						
		// Internal temporary storage
		private var username:String;
		private var role:String;
						
		// Callback function to return the status of the login
		private var loginCallback:Function = null;
		
        public function LogonCommand() {     
        }
		
		public function execute(login: String, password:String, callback:Function=null):void
		{
			loginCallback = callback;
			
			ServiceUtils.send(mdl.session.databaseURL+'logon',
							  this,
							  {username: login, password: password});
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
        								username, 
        							    role);				
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
        	
        	var status:String = logon.status;
        	if (status == 'SUCCESS')
        	{
        		Debug.debug("Login as " + logon.username + ": SUCCESS");
        		
        		// Set the session ID
        		mdl.session.sessionid = logon.sessionid;
        		username = logon.username;
        		role = logon.role;
        		
				// Now fetch config information
				var configCommand:ConfigCommand = new ConfigCommand;
				configCommand.execute(configResult);
				
        	} else if (status == 'FAILURE') {
          		Debug.debug("Login: Incorrect username/password");
          		if (loginCallback != null)
          			loginCallback(false, 'Incorrect username/password');      		
        	} else {
        		Debug.debug("Login: Error communicating with server");
        		if (loginCallback != null)
        			loginCallback(false, 'Error communicating with server');
        	}
		}
		
		// Called when the logon request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("Login: Unable to connect to the server");
            if (loginCallback != null)
            	loginCallback(false, "Unable to connect to the server");
		}
	}
}


