package command
{
	import model.GPPerfMonModelLocator;
	import mx.rpc.events.FaultEvent;
	import util.Debug;
	import util.MiscUtils;
	import util.ServiceUtils;
	import mx.rpc.IResponder;
				
	public class SystemHistoricCommand extends BaseCommand implements ICommand, IResponder
	{
		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();	

		// Store parameters to the request
		private var startTime:Date = null;
		private var endTime:Date = null;
		private var detail:String = '';
		
        public function SystemHistoricCommand() {     
        }

		public function setParameters(interval:String, start:Date, end:Date=null):void
		{
			startTime = start;
			endTime = end;
			detail = interval;			
		}
		
		public function execute():void
		{
			var req:String = '?start=' + MiscUtils.formatDateToString(startTime.time);
			if (endTime)
				req += '&end=' + MiscUtils.formatDateToString(endTime.time);
			req += '&interval=' + detail;
			
			ServiceUtils.send(mdl.session.webServiceURL+'system'+req, this);
		}
        							    
		// Called when the SystemNow request returns successfully
		public function result(data:Object):void
		{
			mdl.session.connectionGood();
			
			var result:XML = XML(data.result);
			
			if (result.name() == 'system')
				mdl.system.systemHistoric(detail, startTime, endTime, result);
			else
				Debug.debug("SystemHistoric error: " + result);	
				
		 	executeChainedCommand(); 
		}
		
		// Called when the SystemNow request returns unsuccessfully
		public function fault(info:Object):void
		{
			var faultEvent : FaultEvent = info as FaultEvent;
			Debug.debug("SystemHistoric fault: " + faultEvent.toString());
			mdl.session.connectionBad();
			
		 	executeChainedCommand(); 
		}
	}
}


