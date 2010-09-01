package model
{
	[Bindable]
	public class Resources
	{
		[Embed(source="../assets/img/top_graphic.jpg")]	
		public static var top_graphic:Class;
	
		[Embed(source="../assets/img/bot_graphic.jpg")]	
		public static var bot_graphic:Class;
		
		[Embed(source="../assets/img/Greenplum_Logo_320w.jpg")]	
		public static var gp_logo_med:Class;

		[Embed(source="../assets/img/Greenplum_Logo_140w.png")]	
		public static var gp_logo_sml:Class;
		
		[Embed(source='../assets/img/exclamation.png')]
   		public static var confirmIcon:Class;
 
 		[Embed(source='../assets/img/Magnifying-Glass-16x16.png')]
   		public static var magGlass:Class;

 		[Embed(source='../assets/img/spyglass_cursor.png')]
   		public static var spyglassCursor:Class;
   		[Embed(source='../assets/img/DBUnbalanced_icon.png')]   		public static var DBUnbalancedIcon:Class;   		   		[Embed(source='../assets/img/DBDegrade_Icon.png')]   		public static var DBDegradeIcon:Class;
   		  				
		public static var copyright:String = "Copyright (c) 2008-2010 Greenplum, Inc. All Rights Reserved";

		public static var copyrightShort:String = "(c) 2008-2010 Greenplum, Inc.";
		
		public static var copyright2:String = "Copyright (c) 2008-2010 Greenplum, Inc.\nAll Rights Reserved";
		
		private var __version:String;// = "Version 2.0.0.0b";
		
		//this has the label "Version" prefixed to the version number
		private var __versionString:String;
		
		//We might want to store the type of this gpperfmon instance (whether appliance or not)
		// in a different manner. However, since we dont know what else could be there, we will
		// simply store in a boolean.
		private var __applianceFlag:Boolean = false ;
		
		private var __fakeDataFlag:Boolean = false ;
		
		//holds the singleton instance of the class
		private static var __instance:Resources;
		
		/**
		 * Constructor 
		 */		
		public function Resources(){
			if(__instance == null){
				__instance = this;
			}
		}
		
		//--------------------------
		//PROPERTIES
		//--------------------------
		/**
		 * Returns the Singleton instance of Resources
		 */
		public static function get instance():Resources{
			if (__instance == null){
				__instance = new Resources();
			}
			return __instance;
		}		
		
		/**
		 * Sets/Gets the version of the server 
		 * @param val
		 */		
		public function set version(val:String):void{
			__version = val;
			
			//set the version string
			this.versionString = "Version " + val;
		}	
		
		public function get version():String{
			return __version;
		} 
		
		/**
		 * Sets/Gets the version of the server 
		 * @param val
		 */		
		public function get versionString():String{
			return __versionString;
		}		
		
		public function set versionString(val:String):void{
			__versionString = val;
		}
		
		public function get appliance():Boolean{
			// TODO: Naveen: remove conditional.
			return (__fakeDataFlag) ? true : __applianceFlag;
		}		
		
		public function set appliance(val:Boolean):void{
			__applianceFlag = val;
		}
		
		public function get fakeData():Boolean{
			return __fakeDataFlag;
		}		
		
	}
}
