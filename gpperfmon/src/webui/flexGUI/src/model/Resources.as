package model
{
	[Bindable]
	public class Resources
	{
		[Embed(source="assets/img/top_graphic.jpg")]	
		public static var top_graphic:Class;
	
		[Embed(source="assets/img/bot_graphic.jpg")]	
		public static var bot_graphic:Class;
		
		[Embed(source="assets/img/Greenplum_Logo_320w.jpg")]	
		public static var gp_logo_med:Class;

		[Embed(source="assets/img/Greenplum_Logo_140w.png")]	
		public static var gp_logo_sml:Class;
		
		[Embed(source='assets/img/exclamation.png')]
   		public static var confirmIcon:Class;
 		
 		[Embed(source='assets/img/minigraph.PNG')]
   		public static var minigraph:Class;
   		  				
		public static var version:String = "Version 2.0a [Eng Preview A]";
		
		public static var copyright:String = "Copyright (c) 2008 Greenplum, Inc. All Rights Reserved";
		
		public static var copyright2:String = "Copyright (c) 2008 Greenplum, Inc.\nAll Rights Reserved";
	}
}