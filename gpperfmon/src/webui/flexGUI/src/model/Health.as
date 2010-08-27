package model
{
	import mx.collections.ArrayCollection;
	import mx.collections.XMLListCollection;
	import util.Debug;
	import util.MiscUtils;
	
	import value.DatabaseSample;
		
	[Bindable]
	public class Health
	{
		public static const STATUS_NORMAL:String = "normal" ;
		public static const STATUS_ERROR:String = "error" ;
		public static const STATUS_WARNING:String = "warnings";
		public static const STATUS_UNKNOWN:String = "unknown";
		public static const STATUS_NORESPONSE:String = "unreachable";
		
		public static const STATUS_LABEL_NORMAL:String = "Normal" ;
		public static const STATUS_LABEL_ERROR:String = "Error" ;
		public static const STATUS_LABEL_WARNING:String = "Warnings";
		public static const STATUS_LABEL_UNKNOWN:String = "Unknown";
		public static const STATUS_LABEL_NORESPONSE:String = "Unreachable";
		
		public static var statuses:Array;
		public static var statusLabels:Array;


		// Reference to the model
		private var mdl:GPPerfMonModelLocator;

		public var summary:XML;
		
		public function Health(_mdl:GPPerfMonModelLocator):void
		{
			mdl = _mdl;
			statuses = new Array();
			statuses[STATUS_NORMAL] = STATUS_NORMAL ;
			statuses[STATUS_ERROR] = STATUS_ERROR ;
			statuses[STATUS_WARNING] = STATUS_WARNING ;
			statuses[STATUS_UNKNOWN] = STATUS_UNKNOWN ;
			statuses[STATUS_NORESPONSE] = STATUS_NORESPONSE ;

			statusLabels = new Array();
			statusLabels[STATUS_NORMAL] = STATUS_LABEL_NORMAL ;
			statusLabels[STATUS_ERROR] = STATUS_LABEL_ERROR ;
			statusLabels[STATUS_WARNING] = STATUS_LABEL_WARNING ;
			statusLabels[STATUS_UNKNOWN] = STATUS_LABEL_UNKNOWN ;
			statusLabels[STATUS_NORESPONSE] = STATUS_LABEL_NORESPONSE ;
		}
		
		public function setHealthSummary(_summary:XML):void
		{
			summary = _summary ;
			
			var lastUpdateTimeStamp:int = int(summary.LastUpdateTime);
			var date:Date=new Date(lastUpdateTimeStamp * 1000);
			summary.LastUpdateTimeString = date.toLocaleString();				

			var categoriesXML:XML = XML(summary.CategoryView);
			for(var i:int = 0; i < categoriesXML.Category.length();  i++) {
				var catXML:XML = XML(categoriesXML.Category[i]);
				
				// add the total number as a new element.
				catXML.@Total = catXML.Line.length();
				
				var statusSummaryList:XMLList = new XMLList();
				var ssListCol:XMLListCollection = new XMLListCollection(statusSummaryList);
				for each (var key:String in statuses) {
					var statusSummary:XML = <Summary></Summary>;
					statusSummary.@Status = key;
					statusSummary.@Count = catXML.Line.(Status.toLowerCase() == key).length();
					ssListCol.addItem(statusSummary);
				}
				
				catXML.Summaries = statusSummaryList;
			}
		}
		
		public static function statusLabel(statusLabelKey:String):String {
			// When localization is needed, change here. But the statuses array should always contain key=key pair as key=value.
			return statusLabels[statusLabelKey];
		}
		
	}
}
