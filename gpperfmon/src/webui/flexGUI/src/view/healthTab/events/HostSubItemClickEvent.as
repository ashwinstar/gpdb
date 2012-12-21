package view.healthTab.events
{
	import flash.events.Event;
	
	import view.healthTab.HostStatusItem;

	public class HostSubItemClickEvent extends Event
	{
		
		public static const HOSTSUBITEM_CLICK:String = "hostSubItemClick";
		public var hostItemRef:HostStatusItem;
		
		public function HostSubItemClickEvent(type:String, bubbles:Boolean=false, cancelable:Boolean=false){
			super(type, bubbles, cancelable);
		}
		
		override public function clone():Event{
			return new HostSubItemClickEvent(type);
		}
		
	}
}