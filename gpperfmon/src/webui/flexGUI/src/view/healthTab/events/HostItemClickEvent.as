     package view.healthTab.events
{
	import flash.events.Event;
	
	import view.healthTab.HostItem;

	public class HostItemClickEvent extends Event
	{
		
		public static const HOSTITEM_CLICK:String = "hostItemClick";
		public var hostItemRef:HostItem;
		
		public function HostItemClickEvent(type:String, bubbles:Boolean=false, cancelable:Boolean=false){
			super(type, bubbles, cancelable);
		}
		
		override public function clone():Event{
			return new HostItemClickEvent(type);
		}
		
	}
}