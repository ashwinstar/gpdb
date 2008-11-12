package model
{
	import flash.events.TimerEvent;
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
		import command.QueriesNowCommand;
		import command.SystemHistoricCommand;
		
		public function setDatabaseConnection(name:String, port:String, 
											  useHTTPS:Boolean = true):void
		{
			databaseName = name;
			databasePort = port;
			usingHTTPS = useHTTPS;
			
			if (useHTTPS)
			{
				databaseURL = "https://" + databaseName + ":" + databasePort + "/";
			}
			else
			{
				databaseURL = "http://" + databaseName + ":" + databasePort + "/";
			}
			
			Debug.debug("DatabaseURL = " + databaseURL);
		}
			
		// Attempts to log the user into the system
		public function login(loginTI:String, passwordTI:String, callback:Function):void
		{
			uiCallback = callback;
			
			var logonCommand:LogonCommand = new LogonCommand;
			logonCommand.execute(loginTI, passwordTI, loginResult);
		}

	    // Callback from login command
	    public function loginResult(success:Boolean, message:String,
	    							_username:String = '', 
	    							_role:String = ''):void
	    {
			if (success)
			{
				username = _username;
				if (_role == 'superuser')
				  isSuperuser = true;
				else
				  isSuperuser = false;
				loggedIn = 1;		
				
				// Setup the quantum timer (and call it once for good measure)
				initTimeVals();
				onQuantumTimer(null);
				quantumTimer = new Timer(monitorQuantum * 1000);
				quantumTimer.addEventListener(TimerEvent.TIMER, onQuantumTimer);
				quantumTimer.start();
				
				//Issue request to get the last 6hrs of 1min system data
				var oneMinRequest:SystemHistoricCommand = new SystemHistoricCommand;
				oneMinRequest.execute('1min', timeArray[Session.SIX_HOURS_AGO]);						
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
			// Stop the quantum timer
			quantumTimer.stop();
			
			var logoffCommand:LogoffCommand = new LogoffCommand;
			logoffCommand.execute();
			resetState();
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
		}
		
		// Called if we hit an access denied -- e.g. session exired
		public function accessDenied():void
		{
			if (!accessDeniedVisible)
			{
				accessDeniedVisible = true;
				
				var a:Alert = Alert.show("Your session has expired. Do you wish to logout?", 
				    "Session Expired", 
					Alert.YES|Alert.NO, null, accessDeniedPopupHandler, 
					Resources.confirmIcon, Alert.NO);
				
				// modify the look of the Alert box
				a.setStyle("backgroundColor", 0xffffff);
				a.setStyle("backgroundAlpha", 0.75);
				a.setStyle("borderColor", 0xefffe0);
				a.setStyle("borderAlpha", 0.90);
				a.setStyle("color", 0x000000); // text color
			}
		}
	
		private function accessDeniedPopupHandler(event:CloseEvent):void
  		{
  			accessDeniedVisible = false;
  			if (event.detail==Alert.YES)
  			{
    			logout()
    		}
  		}
		
		// Called every quantum tick
		public function onQuantumTimer(event:TimerEvent):void
		{
			setTimeVals();
			var systemNowCommand:SystemNowCommand = new SystemNowCommand;
			systemNowCommand.execute();
			var queriesNowCommand:QueriesNowCommand = new QueriesNowCommand;
			queriesNowCommand.execute();
		}

		// Initializes the time values
		public function initTimeVals():void
		{
			var latestTick:Number = MiscUtils.prevMinAsNumber();
			timeArray.addItemAt(new Date(latestTick), LATEST_TICK);
			timeArray.addItemAt(new Date(latestTick-MiscUtils.HOUR), ONE_HOUR_AGO);
			timeArray.addItemAt(new Date(latestTick-(MiscUtils.HOUR*6)), SIX_HOURS_AGO);
			timeArray.addItemAt(new Date(latestTick-MiscUtils.DAY), ONE_DAY_AGO);
			timeArray.addItemAt(new Date(latestTick-(MiscUtils.DAY*7)), SEVEN_DAYS_AGO);
			timeArray.addItemAt(new Date(latestTick-(MiscUtils.DAY*14)), TWO_WEEKS_AGO);
			timeArray.addItemAt(new Date(latestTick-(MiscUtils.MONTH*3)), THREE_MONTHS_AGO);
			timeArray.addItemAt(new Date(latestTick-MiscUtils.YEAR), ONE_YEAR_AGO);
			timeArray.addItemAt(new Date(latestTick-(MiscUtils.MINUTE*15)), FIFTEEN_MINS_AGO);		}

		// Sets bindable time values
		public function setTimeVals():void
		{
			var latestTick:Number = MiscUtils.prevMinAsNumber();
			if ((timeArray.getItemAt(LATEST_TICK) as Date).time < latestTick)						
			{
				timeArray.setItemAt(new Date(latestTick), LATEST_TICK);
				timeArray.setItemAt(new Date(latestTick-MiscUtils.HOUR), ONE_HOUR_AGO);
				timeArray.setItemAt(new Date(latestTick-(MiscUtils.HOUR*6)), SIX_HOURS_AGO);
				timeArray.setItemAt(new Date(latestTick-MiscUtils.DAY), ONE_DAY_AGO);
				timeArray.setItemAt(new Date(latestTick-(MiscUtils.DAY*7)), SEVEN_DAYS_AGO);
				timeArray.setItemAt(new Date(latestTick-(MiscUtils.DAY*14)), TWO_WEEKS_AGO);
				timeArray.setItemAt(new Date(latestTick-(MiscUtils.MONTH*3)), THREE_MONTHS_AGO);
				timeArray.setItemAt(new Date(latestTick-MiscUtils.YEAR), ONE_YEAR_AGO);
				timeArray.setItemAt(new Date(latestTick-(MiscUtils.MINUTE*15)), FIFTEEN_MINS_AGO);
			}
		}

		// Called when an operation fails
		public function connectionBad():void 
		{
			connectionStatus = 'disconnected';
		}

		// Called when an operation succeeds
		public function connectionGood():void 
		{
			connectionStatus = 'connected';
		}

        ////////////////////////////////////////////////
        // Cross-session values
        
        // IP address or name
		public var databaseName:String = "";
		
		// Port
		public var databasePort:String = "";

        // URL
		public var databaseURL:String = "";
		
		// Are we using HTTPS?
		public var usingHTTPS:Boolean = true;

        // What is the state of the connection?
        public var connectionStatus:String = 'connected';
		
		////////////////////////////////////////////////
        // Per-session values
        	
		// Is a user logged in?
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

		////////////////////////////////////////////////
        // Time values
        
        public var timeArray:ArrayCollection = new ArrayCollection();
        
        // Note: these numbers line up with the togglebar values
        public static const LATEST_TICK:int = 0; 
        public static const ONE_HOUR_AGO:int = 1; 
        public static const SIX_HOURS_AGO:int = 2; 
        public static const ONE_DAY_AGO:int = 3; 
        public static const SEVEN_DAYS_AGO:int = 4; 
        public static const TWO_WEEKS_AGO:int = 5; 
        public static const THREE_MONTHS_AGO:int = 6; 
        public static const ONE_YEAR_AGO:int = 7; 
        public static const FIFTEEN_MINS_AGO:int = 8;
        
		////////////////////////////////////////////////
        // Private values
		
		// Callback function to notify the UI of success/failure of login
		private var uiCallback:Function = null; 
		
		// Are we showing the access denied prompt
		private var accessDeniedVisible:Boolean = false; 
	}
}