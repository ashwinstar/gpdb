package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.ServiceUtils;
	import mx.rpc.IResponder;
  	import mx.controls.Alert;
  	import model.Resources;
  	import model.Health;
				
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
			if (Resources.instance.fakeData) {
				fakeExecute();
			} else {
				ServiceUtils.send(mdl.session.webServiceURL+'health', this);
			}
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
		 		callBack();
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
		
		// TODO: Naveen: remove (or keep?) when done with testing.
		// This function will send fake XML to fake a non-existent command.
		private function fakeExecute():void {
			var data:Object = new Object();
			var resultXML:XML = <Health>
							<LastUpdateTime>1283185541</LastUpdateTime>
							<CategoryView>
							<Category Name="Master Server">
							<Line>
							    <HostName>mdw</HostName>
							    <Status>error</Status>
							</Line>
							</Category>
							<Category Name="Segment Server">
							<Line>
							    <HostName>sdw2</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw11</HostName>
							    <Status>unreachable</Status>
							</Line>
							<Line>
							    <HostName>sdw12</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw4</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw13</HostName>
							    <Status>unreachable</Status>
							</Line>
							<Line>
							    <HostName>sdw14</HostName>
							    <Status>unreachable</Status>
							</Line>
							<Line>
							    <HostName>sdw6</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw7</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw16</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw8</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw9</HostName>
							    <Status>error</Status>
							</Line>
							<Line>
							    <HostName>sdw10</HostName>
							    <Status>unreachable</Status>
							</Line>
							<Line>
							    <HostName>sdw1</HostName>
							    <Status>error</Status>
							</Line>
							</Category>
							</CategoryView>
							</Health>;
			var curDate:Date = new Date();
			resultXML.LastUpdateTime = int(curDate.time/1000);
			
			var testList:XMLList = resultXML.CategoryView.Category.(@Name == "Master Server");
			var testCat:XML = testList[0];
			testCat.Line.HostName = "mdw" + (curDate.time % 1000);
			testCat.Line.Status = Health.statusList[(int(Math.random() * 5) % 5)];
			
			data.result = resultXML;
			result(data);
		}
	}
}


