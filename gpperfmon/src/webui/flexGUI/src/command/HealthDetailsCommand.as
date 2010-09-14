package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.IResponder;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.ServiceUtils;
	import model.Resources;
				
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
			if (Resources.instance.applianceSimulator) {
				// If we are simulating the appliance, just get the hard coded response.
				fakeExecute();
			} else {
				ServiceUtils.send(mdl.session.webServiceURL+'healthdetails'+req, this);
			}
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
		
		
		/**
		 * Normally, the HealthCommand and HealthDetailsCommand are only sent when
		 * we are dealing with an appliance. However, to simulate the appliance for
		 * development and testing purposes, we can set the
		 * Resources.instance.applianceSimulator flag, and the following hard-coded
		 * data will be returned for the command.
		 */
		private function fakeExecute():void {
			var data:Object = new Object();
			data.result = <HealthDetails>
							<Host Name='mdw'>
							<Line>    
							    <ComponentName>System Manufacturer</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.300.10.1.8.1</ObjectID>
							    <Status>normal</Status>
							    <Message>Dell Inc.</Message>
							</Line>
							<Line>    
							    <ComponentName>System Model</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.300.10.1.9.1</ObjectID>
							    <Status>normal</Status>
							    <Message>PowerEdge R610</Message>
							</Line>
							<Line>    
							    <ComponentName>OS Name</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.400.10.1.6.1</ObjectID>
							    <Status>normal</Status>
							    <Message>Red Hat Enterprise Linux Server</Message>
							</Line>
							<Line>    
							    <ComponentName>OS Version</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.400.10.1.7.1</ObjectID>
							    <Status>normal</Status>
							    <Message>release 5.5 (Tikanga) Kernel 2.6.18-194.3.1.el5 (x86_64)</Message>
							</Line>
							<Line>    
							    <ComponentName>Power Supply Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.600.12.1.5.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Power Supply Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.600.12.1.5.1.2</ObjectID>
							    <Status>error</Status>
							    <Message>critical</Message>
							</Line>
							<Line>    
							    <ComponentName>Battery Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.600.50.1.5.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 1A RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 2A RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 3A RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 5A RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 6A RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 1B RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 7</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.7</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 2B RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 8</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.8</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 3B RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 9</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.9</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 4B RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 10</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.10</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 5B RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 11</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.11</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 6B RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Name 12</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.8.1.12</ObjectID>
							    <Status>normal</Status>
							    <Message>System Board FAN MOD 4A RPM</Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 7</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.7</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 8</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.8</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 9</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.9</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 10</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.10</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 11</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.11</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cooling Device Status 12</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.700.12.1.5.1.12</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Processor Version 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.30.1.16.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>Intel(R) Xeon(R) CPU           X5680  @ 3.33GHz Stepping 2</Message>
							</Line>
							<Line>    
							    <ComponentName>Processor Version 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.30.1.16.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>Intel(R) Xeon(R) CPU           X5680  @ 3.33GHz Stepping 2</Message>
							</Line>
							<Line>    
							    <ComponentName>Processor Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.30.1.5.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Processor Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.30.1.5.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Size 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.13.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>192</Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Size 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.13.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>12288</Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Size 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.13.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message>1536</Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Size 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.13.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message>192</Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Size 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.13.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message>1536</Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Size 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.13.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message>12288</Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.5.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.5.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Status 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.5.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Status 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.5.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Status 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.5.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Cache Device Status 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.40.1.5.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>OS Memory Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.400.20.1.4.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Memory Device Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.50.1.5.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Memory Device Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.50.1.5.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Memory Device Status 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.50.1.5.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Memory Device Status 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.50.1.5.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Memory Device Status 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.50.1.5.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Memory Device Status 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.50.1.5.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.5.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom 1GbE BASE-T MC Server Adapter</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.5.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom 1GbE BASE-T MC Server Adapter</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.5.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom 1GbE BASE-T MC Server Adapter</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.5.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom 1GbE BASE-T MC Server Adapter</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.5.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message>1010/1020/1007 10Gbps CNA - LL</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.5.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message>1010/1020/1007 10Gbps CNA - LL</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Vendor 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.7.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom Corp</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Vendor 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.7.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom Corp</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Vendor 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.7.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom Corp</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Vendor 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.7.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message>Broadcom Corp</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Vendor 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.7.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message>Brocade Communications Systems, Inc.</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Vendor 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.7.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message>Brocade Communications Systems, Inc.</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device IP 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.11.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>172.28.4.250</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device IP 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.11.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>10.6.193.46</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device IP 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.11.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message>172.28.8.250</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device IP 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.11.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message>172.28.12.250</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Subnet Mask 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.12.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>255.255.248.0</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Subnet Mask 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.12.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>255.255.252.0</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Subnet Mask 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.12.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message>255.255.252.0</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Subnet Mask 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.12.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message>255.255.252.0</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.4.1.1</ObjectID>
							    <Status>normal</Status>
							    <Message>connected</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.4.1.2</ObjectID>
							    <Status>normal</Status>
							    <Message>connected</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Status 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.4.1.3</ObjectID>
							    <Status>normal</Status>
							    <Message>driver disabled</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Status 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.4.1.4</ObjectID>
							    <Status>normal</Status>
							    <Message>driver disabled</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Status 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.4.1.5</ObjectID>
							    <Status>normal</Status>
							    <Message>connected</Message>
							</Line>
							<Line>    
							    <ComponentName>Network Device Status 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10892.1.1100.90.1.4.1.6</ObjectID>
							    <Status>normal</Status>
							    <Message>connected</Message>
							</Line>
							<Line>    
							    <ComponentName>Controller Name 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.1.1.2.1</ObjectID>
							    <Status>normal</Status>
							    <Message>PERC H700 Integrated</Message>
							</Line>
							<Line>    
							    <ComponentName>Controller Vendor 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.1.1.3.1</ObjectID>
							    <Status>normal</Status>
							    <Message>DELL</Message>
							</Line>
							<Line>    
							    <ComponentName>Controller Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.1.1.5.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Controller Battery Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.15.1.5.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Name 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.2.1</ObjectID>
							    <Status>normal</Status>
							    <Message>boot</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Name 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.2.2</ObjectID>
							    <Status>normal</Status>
							    <Message>swap</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Name 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.2.3</ObjectID>
							    <Status>normal</Status>
							    <Message>data</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Device Name 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.3.1</ObjectID>
							    <Status>normal</Status>
							    <Message>/dev/sda</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Device Name 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.3.2</ObjectID>
							    <Status>normal</Status>
							    <Message>/dev/sdb</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Device Name 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.3.3</ObjectID>
							    <Status>normal</Status>
							    <Message>/dev/sdc</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.19.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.19.2</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Status 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.19.3</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Layout 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.13.1</ObjectID>
							    <Status>normal</Status>
							    <Message>RAID-5</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Layout 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.13.2</ObjectID>
							    <Status>normal</Status>
							    <Message>RAID-5</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Layout 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.13.3</ObjectID>
							    <Status>normal</Status>
							    <Message>RAID-5</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk MB 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.6.1</ObjectID>
							    <Status>normal</Status>
							    <Message>50165</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk MB 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.6.2</ObjectID>
							    <Status>normal</Status>
							    <Message>50165</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk MB 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.6.3</ObjectID>
							    <Status>normal</Status>
							    <Message>2758548</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Write Policy 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.10.1</ObjectID>
							    <Status>warning</Status>
							    <Message>Enabled Always (SAS)</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Write Policy 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.10.2</ObjectID>
							    <Status>warning</Status>
							    <Message>Enabled Always (SAS)</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Write Policy 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.10.3</ObjectID>
							    <Status>warning</Status>
							    <Message>Enabled Always (SAS)</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Read Policy 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.11.1</ObjectID>
							    <Status>warning</Status>
							    <Message>LSI No Read Ahead</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Read Policy 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.11.2</ObjectID>
							    <Status>warning</Status>
							    <Message>LSI No Read Ahead</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk Read Policy 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.11.3</ObjectID>
							    <Status>warning</Status>
							    <Message>LSI No Read Ahead</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk State 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.4.1</ObjectID>
							    <Status>normal</Status>
							    <Message>Ready</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk State 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.4.2</ObjectID>
							    <Status>normal</Status>
							    <Message>Ready</Message>
							</Line>
							<Line>    
							    <ComponentName>Virtual Disk State 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.140.1.1.4.3</ObjectID>
							    <Status>normal</Status>
							    <Message>Ready</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Name 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.2.1</ObjectID>
							    <Status>normal</Status>
							    <Message>Physical Disk 0:0:0</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Name 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.2.2</ObjectID>
							    <Status>normal</Status>
							    <Message>Physical Disk 0:0:1</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Name 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.2.3</ObjectID>
							    <Status>normal</Status>
							    <Message>Physical Disk 0:0:2</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Name 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.2.4</ObjectID>
							    <Status>normal</Status>
							    <Message>Physical Disk 0:0:3</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Name 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.2.5</ObjectID>
							    <Status>normal</Status>
							    <Message>Physical Disk 1:0:4</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Name 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.2.6</ObjectID>
							    <Status>normal</Status>
							    <Message>Physical Disk 1:0:5</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Vendor 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.3.1</ObjectID>
							    <Status>normal</Status>
							    <Message>DELL(tm)</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Vendor 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.3.2</ObjectID>
							    <Status>normal</Status>
							    <Message>DELL(tm)</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Vendor 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.3.3</ObjectID>
							    <Status>normal</Status>
							    <Message>DELL(tm)</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Vendor 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.3.4</ObjectID>
							    <Status>normal</Status>
							    <Message>DELL(tm)</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Vendor 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.3.5</ObjectID>
							    <Status>normal</Status>
							    <Message>DELL(tm)</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Vendor 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.3.6</ObjectID>
							    <Status>normal</Status>
							    <Message>DELL(tm)</Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Status 1</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.23.1</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Status 2</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.23.2</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Status 3</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.23.3</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Status 4</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.23.4</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Status 5</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.23.5</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							<Line>    
							    <ComponentName>Array Disk Status 6</ComponentName>
							    <ObjectID>.1.3.6.1.4.1.674.10893.1.20.130.4.1.23.6</ObjectID>
							    <Status>normal</Status>
							    <Message></Message>
							</Line>
							</Host>
						</HealthDetails>;
			result(data);
		}
	}
}


