package view.healthTab.events
{
		import flash.events.Event;
	public class DetailsIconClick extends Event
	{
		public static const DetailsIcon_CLICK:String = "detailsIconClick.";
		public var itemXML:XML;
		public function DetailsIconClick(type:String, bubbles:Boolean=false, cancelable:Boolean=false)
		{
			super(type, bubbles, cancelable);
		}
        override public function clone():Event{
			return new DetailsIconClick(type);
		}
	}
}