package model
{
	import flash.events.TimerEvent;
	import flash.net.URLRequest;
	import flash.net.navigateToURL;
	import flash.utils.Timer;
	
	import mx.collections.ArrayCollection;
	import mx.controls.Alert;
	import mx.events.CloseEvent;
  		
	[Bindable]
	public class Session
	{
		import util.Debug;
		import util.MiscUtils;
		import command.LogonCommand;
		import command.LogoffCommand;
		import command.SystemNowCommand;
		import command.DatabaseNowCommand;
		import command.QueriesNowCommand;
		import command.SystemHistoricCommand;
		import command.DatabaseHistoricCommand;
		import command.HealthCommand;
				
		public function setDatabaseConnection(protocol:String, 
		                                      address:String, port:String,
		                                      serverName:String):void
		{
			databaseName = serverName;
			
			webServiceURL = protocol + "://" + address + ":" + port + "/";
		
			Debug.debug("databaseName = " + databaseName);
			Debug.debug("webServiceURL = " + webServiceURL);
		}
			
		// Attempts to log the user into the system
		public function login(loginTI:String, passwordTI:String, callback:Function):void
		{
			uiCallback = callback;
			
			var logonCommand:LogonCommand = new LogonCommand;
			logonCommand.setParameters(loginTI, passwordTI, loginResult);
			logonCommand.execute();
		}

	    // Callback from login command
	    public function loginResult(success:Boolean, message:String,
	    							_username:String = '', 
	    							_role:String = '', _appliance:Boolean = false):void
	    {
			if (success)
			{
				username = _username;
				if (_role == 'superuser')
				  isSuperuser = true;
				else
				  isSuperuser = false;
				loggedIn = 1;
				
				Resources.instance.appliance = _appliance;	
				
				// Setup the quantum timer (and call it once for good measure)
				initTimeVals();
				
				//Issue request to get the last 6hrs of 1min system and db data
				var oneMinRequestSys:SystemHistoricCommand = new SystemHistoricCommand;
				oneMinRequestSys.setParameters('1min', timeArray[Session.SIX_HOURS_AGO]);	
				var oneMinRequestDB:DatabaseHistoricCommand = new DatabaseHistoricCommand;
				oneMinRequestDB.setParameters('1min', timeArray[Session.SIX_HOURS_AGO]);
				oneMinRequestSys.chainCommand(oneMinRequestDB);
				oneMinRequestSys.execute();
				
				onQuantumTimer(null);
				quantumTimer = new Timer(monitorQuantum * 1000);
				quantumTimer.addEventListener(TimerEvent.TIMER, onQuantumTimer);
				quantumTimer.start();
				
			}
			else
			{
				resetState();
			}
	    	
	    	if (uiCallback != null)
	    		uiCallback(success, message);
	    	uiCallback = null;
	    }
		
	   
		// Attempts to log the user out of the system
		public function logout():void
		{
			if (loggedIn == 1)
			{
				// Stop the quantum timers
				if (quantumTimer)
					quantumTimer.stop();
				if (queryPlanTimer)
				   queryPlanTimer.stop();
			
				var logoffCommand:LogoffCommand = new LogoffCommand;
				logoffCommand.execute();
				
				// Switch to the logged-out page (in case the reload of the plugin doesn't work)
				loggedIn=2;
				GPPerfMonModelLocator.releaseInstanceData();
				// Now try to reload the plugin
				navigateToURL(new URLRequest(webServiceURL),'_self');
			}
		}

        // Reset all per-session values
		public function resetState():void
		{
			loggedIn = 0;
			username = '';
			isSuperuser = false;
			sessionid = '';
			config = null;
			monitorQuantum = 0;
			quantumTimer = null;
			queryPlanTimer = null;
		}
		
		// Called if we hit an access denied -- e.g. session expired
		public function accessDenied():void
		{
			if (!accessDeniedVisible)
			{
				accessDeniedVisible = true;
				
				var a:Alert = Alert.show("Your session has expired.\nClick to return to the logon screen.", 
				    "Session Expired", 
					Alert.OK, null, accessDeniedPopupHandler, 
					Resources.confirmIcon, Alert.OK);
				
				// modify the look of the Alert box
				a.setStyle("backgroundColor", 0xffffff);
				a.setStyle("backgroundAlpha", 0.75);
				a.setStyle("borderColor", 0xefffe0);
				a.setStyle("borderAlpha", 0.90);
				a.setStyle("color", 0x000000); // text color

				// Stop the quantum timers
				if (quantumTimer)
					quantumTimer.stop();
				if (queryPlanTimer)
				   queryPlanTimer.stop();
			}
		}
	
		private function accessDeniedPopupHandler(event:CloseEvent):void
  		{
  			accessDeniedVisible = false;
    		logout();
  		}
		
		// Called every quantum tick
		public function onQuantumTimer(event:TimerEvent):void
		{
			var systemNowCommand:SystemNowCommand = new SystemNowCommand;
			var databaseNowCommand:DatabaseNowCommand = new DatabaseNowCommand;
			var queriesNowCommand:QueriesNowCommand = new QueriesNowCommand;
			systemNowCommand.chainCommand(databaseNowCommand);
			databaseNowCommand.chainCommand(queriesNowCommand);
			
			/* For now, even though the call is going repeatedly, no one will be
			** refreshing the views based on the results.
			*/
			if (Resources.instance.appliance) {
				var healthCommand:HealthCommand = new HealthCommand;
				queriesNowCommand.chainCommand(healthCommand);
			}
			
			systemNowCommand.execute();
		}

		// Initializes the time values
		public function initTimeVals():void
		{
			var prevMin:Number = MiscUtils.prevMinAsNumber(mostRecentTick);
			timeArray.addItemAt(new Date(prevMin-MiscUtils.HOUR), ONE_HOUR_AGO);
			timeArray.addItemAt(new Date(prevMin-(MiscUtils.HOUR*6)), SIX_HOURS_AGO);
			timeArray.addItemAt(new Date(prevMin-MiscUtils.DAY), ONE_DAY_AGO);
			timeArray.addItemAt(new Date(prevMin-(MiscUtils.DAY*7)), SEVEN_DAYS_AGO);
			timeArray.addItemAt(new Date(prevMin-(MiscUtils.DAY*14)), TWO_WEEKS_AGO);
			timeArray.addItemAt(new Date(prevMin-(MiscUtils.MONTH*3)), THREE_MONTHS_AGO);
			timeArray.addItemAt(new Date(prevMin-MiscUtils.YEAR), ONE_YEAR_AGO);
			timeArray.addItemAt(new Date(prevMin), LATEST_TICK);
			timeArray.addItemAt(new Date(prevMin-(MiscUtils.MINUTE*15)), FIFTEEN_MINS_AGO);
		}

		// Sets bindable time values
		public function setTimeVals(serverDate:Date):void
		{
			// Set the most recent tick based on the server's datetime
			mostRecentTick = serverDate;	
			
			var prevMin:Number = MiscUtils.prevMinAsNumber(mostRecentTick);
			if ((timeArray.getItemAt(LATEST_TICK) as Date).time != prevMin)						
			{
				timeArray.disableAutoUpdate();
				timeArray.setItemAt(new Date(prevMin-MiscUtils.HOUR), ONE_HOUR_AGO);
				timeArray.setItemAt(new Date(prevMin-(MiscUtils.HOUR*6)), SIX_HOURS_AGO);
				timeArray.setItemAt(new Date(prevMin-MiscUtils.DAY), ONE_DAY_AGO);
				timeArray.setItemAt(new Date(prevMin-(MiscUtils.DAY*7)), SEVEN_DAYS_AGO);
				timeArray.setItemAt(new Date(prevMin-(MiscUtils.DAY*14)), TWO_WEEKS_AGO);
				timeArray.setItemAt(new Date(prevMin-(MiscUtils.MONTH*3)), THREE_MONTHS_AGO);
				timeArray.setItemAt(new Date(prevMin-MiscUtils.YEAR), ONE_YEAR_AGO);
				timeArray.setItemAt(new Date(prevMin), LATEST_TICK);
				timeArray.setItemAt(new Date(prevMin-(MiscUtils.MINUTE*15)), FIFTEEN_MINS_AGO);
				timeArray.enableAutoUpdate();
			}
		}

		// Called when an operation fails
		public function connectionBad():void 
		{
			connectionStatus = 'Disconnected';
		}

		// Called when an operation succeeds
		public function connectionGood():void 
		{
			connectionStatus = 'Connected';
		}

        ////////////////////////////////////////////////
        // Cross-session values
        
        // IP address or name
		public var databaseName:String = "";

        // URL
		public var webServiceURL:String = "";

        // What is the state of the connection?
        public var connectionStatus:String = 'Connected';
		
		// What is the application URL?
		public var appURL:String = '';
		
		////////////////////////////////////////////////
        // Per-session values
        	
		// Is a user logged in (0=no, 1=yes, 2=loggedOut)?
		public var loggedIn:int = 0;
				
		// Login ID of the user
		public var username:String = "";
		
		// Is the user a superuser?
		public var isSuperuser:Boolean = false;
		
		// Session ID
		public var sessionid:String = '';
		
		// The configuration file in XML
		public var config:XML = null;
		
		// The sampling quantum - from config file
		public var monitorQuantum:int = 0;		
		
		// Timer to trigger per-quantum actions
		private var quantumTimer:Timer = null;

		// Timer to trigger query plan actions
		public var queryPlanTimer:Timer = null;

		////////////////////////////////////////////////
        // Time values
        
        // This value is populated from the system samples we receive from the server
        public var mostRecentTick:Date = new Date();
        
        public var timeArray:ArrayCollection = new ArrayCollection();
        
        // Note: these numbers line up with the list values
        public static const ONE_HOUR_AGO:int = 0; 
        public static const SIX_HOURS_AGO:int = 1; 
        public static const ONE_DAY_AGO:int = 2; 
        public static const SEVEN_DAYS_AGO:int = 3; 
        public static const TWO_WEEKS_AGO:int = 4; 
        public static const THREE_MONTHS_AGO:int = 5; 
        public static const ONE_YEAR_AGO:int = 6; 
        // -- and others
        public static const LATEST_TICK:int = 7; 
        public static const FIFTEEN_MINS_AGO:int = 8;
        
		////////////////////////////////////////////////
        // Private values
		
		// Callback function to notify the UI of success/failure of login
		private var uiCallback:Function = null; 
		
		// Are we showing the access denied prompt
		private var accessDeniedVisible:Boolean = false; 
	}
}