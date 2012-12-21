package model
{
	import mx.collections.ArrayCollection;
	import mx.collections.XMLListCollection;
		
	[Bindable]
	public class Health
	{
		public static const STATUS_NORMAL:String = "normal" ;
		public static const STATUS_ERROR:String = "error" ;
		public static const STATUS_WARNING:String = "warning";
		public static const STATUS_UNKNOWN:String = "unknown";
		public static const STATUS_NORESPONSE:String = "unreachable";
		
		public static const STATUS_LABEL_NORMAL:String = "Normal" ;
		public static const STATUS_LABEL_ERROR:String = "Error" ;
		public static const STATUS_LABEL_WARNING:String = "Warning";
		public static const STATUS_LABEL_UNKNOWN:String = "Unknown";
		public static const STATUS_LABEL_NORESPONSE:String = "Unreachable";
		
		public static const HOSTXMLRECEIVED:String = "HOSTXMLRECEIVED";

		public var statusList:Array;
		public var statusLabels:Array;

		public var consolidatedStatusCounts:ArrayCollection;


		// Reference to the model
		private var mdl:GPPerfMonModelLocator;

		public var summary:XML;
		
		public function Health(_mdl:GPPerfMonModelLocator):void
		{
			mdl = _mdl;

			statusLabels = new Array();
			statusLabels[STATUS_NORMAL] = STATUS_LABEL_NORMAL ;
			statusLabels[STATUS_UNKNOWN] = STATUS_LABEL_UNKNOWN ;
			statusLabels[STATUS_WARNING] = STATUS_LABEL_WARNING ;
			statusLabels[STATUS_ERROR] = STATUS_LABEL_ERROR ;
			statusLabels[STATUS_NORESPONSE] = STATUS_LABEL_NORESPONSE ;
			
			statusList = new Array();
			statusList = [STATUS_NORMAL, STATUS_UNKNOWN, STATUS_WARNING, STATUS_ERROR, STATUS_NORESPONSE];
			
			consolidatedStatusCounts = new ArrayCollection();
			for each (var item:String in statusList) {
				var obj:HealthStatusCount = new HealthStatusCount();
				obj.status = item ;
				obj.statusLabel = statusLabels[item];
				obj.count = 0;
				obj.nonEmpty = false ;
				consolidatedStatusCounts.addItem(obj);
			}
		}
		
		public function setHealthSummary(_summary:XML):void
		{
			var i:int = 0;
			
			summary = _summary ;
			
			var lastUpdateTimeStamp:int = int(summary.LastUpdateTime);
			var date:Date=new Date(lastUpdateTimeStamp * 1000);
			summary.LastUpdateTimeString = date.toLocaleString();
			
			var consolidatedStatus:String = getConsolidatedStatus();
			summary.ConsolidatedStatus = consolidatedStatus;
			summary.ConsolidatedStatusLabel = statusLabel(consolidatedStatus);

			for (i = 0; i < consolidatedStatusCounts.length; i++) {
				var item:HealthStatusCount = HealthStatusCount(consolidatedStatusCounts.getItemAt(i));
				item.count = summary.CategoryView.Category.Line.(Status == item.status).length();
				item.nonEmpty = (item.count > 0);
			}

			var categoriesXML:XML = XML(summary.CategoryView);
			for(i = 0; i < categoriesXML.Category.length();  i++) {
				var catXML:XML = XML(categoriesXML.Category[i]);
				
				// add the total number as a new element.
				catXML.@Total = catXML.Line.length();
				
				var statusSummaryList:XMLList = new XMLList();
				var ssListCol:XMLListCollection = new XMLListCollection(statusSummaryList);
				for (var j:int = 0; j < statusList.length; j++) {
					var statusSummary:XML = <Summary></Summary>;
					statusSummary.@Status = statusList[j];
					statusSummary.@Count = catXML.Line.(Status.toLowerCase() == statusList[j]).length();
					ssListCol.addItem(statusSummary);
				}
				
				catXML.Summaries = statusSummaryList;
			}

			// Notify whoever is interested in this data that the data is received.
			dispatchEvent(new Event(HOSTXMLRECEIVED));
		}
		
		private function getConsolidatedStatus():String {
			var consolidatedStatus:String = STATUS_NORMAL;
			
			if (summary.CategoryView.Category.Line.(Status == STATUS_NORESPONSE).length() > 0) {
				consolidatedStatus = STATUS_NORESPONSE;
			} else if (summary.CategoryView.Category.Line.(Status == STATUS_ERROR).length() > 0) {
				consolidatedStatus = STATUS_ERROR;
			} else if (summary.CategoryView.Category.Line.(Status == STATUS_WARNING).length() > 0) {
				consolidatedStatus = STATUS_WARNING;
			} else if (summary.CategoryView.Category.Line.(Status == STATUS_UNKNOWN).length() > 0) {
				consolidatedStatus = STATUS_UNKNOWN;
			}
		
			return consolidatedStatus;
		}
		
		public static function statusLabel(statusLabelKey:String):String {
			// When localization is needed, change here. But the statuses array should always contain key=key pair as key=value.
			
			var singletonModel:GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();
			
			return (singletonModel != null && singletonModel.health != null) ? singletonModel.health.statusLabels[statusLabelKey] : null;
		}
		
	}
}
