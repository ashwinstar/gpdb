package view.healthTab.events
{
	import flash.events.Event;

	public class HostSubItemSelectEvent extends Event
	{
		
		public static const HOSTSUBITEM_CLICK:String = "hostSubItemClick";
		public var itemXML:XML;
		
		public function HostSubItemSelectEvent(type:String, bubbles:Boolean=false, cancelable:Boolean=false)
		{
			super(type, bubbles, cancelable);
		}
		
		override public function clone():Event{
			return new HostSubItemSelectEvent(type);
		}
		
	}
}