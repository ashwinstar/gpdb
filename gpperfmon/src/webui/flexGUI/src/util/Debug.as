package util
{
	import mx.utils.ObjectUtil;
	
	public class Debug
	{
		public static function debug(val:String):void
		{
			debugBuffer += val + '\n';
			trace('DEBUG: ' + val);
		}
		
		public static function debugObj(val:String, obj:Object):void
		{
			var objVal:String = ObjectUtil.toString(obj);
			debugBuffer += val + ': ' + objVal + '\n';
			trace('DEBUG_OBJ: ' + val);
			trace(obj);
		}
		
		
		// Debug buffer
		[Bindable]
		public static var debugBuffer:String = "";	
	}
}