package view
{
	import mx.controls.treeClasses.TreeItemRenderer;
	
	/**
	 * 
	 * @author vasu@divami.com
	 * 
	 * This class forms as the renderer base for the QueryDetailsPlan's tree node. The actual view 
	 * is defined in 'QueryDetailsPlanTreeNodeDisplay' class. 
	 * 
	 * This class reads the node data, during the tree render, and posts it to the display class. As a custom view is defined,
	 * it suppresses the default label display. 
	 */
	public class QueryDetailPlanTreeNodeRenderer extends TreeItemRenderer
	{
		private var nodeDisplay:QueryDetailPlanTreeNodeDisplay;
		
		public function QueryDetailPlanTreeNodeRenderer(){
			super();
			
			//instantiate the node render UI
			nodeDisplay = new QueryDetailPlanTreeNodeDisplay();
			this.addChild(nodeDisplay);			
		}

		//Gets triggered in each display cycle. It takes care of setting the width and height for the UI
		//and position it beside the icon		
		override protected function updateDisplayList(w:Number, h:Number):void{
			super.updateDisplayList(w, h);
			
			//set the label value as blank as we have a custom UI defined
			super.label.text = "";
						
			nodeDisplay.x = super.label.x + 4;
			nodeDisplay.width = w;
			nodeDisplay.height = h;			
		}
		
		//-------------------
		//PROPERTIES
		//-------------------
		
		override public function set data(value:Object):void{
			super.data = value;
			
			//set the label value as blank as we have a custom UI defined
			super.label.text = "";
			//pass the noide data to the QueryDetailsPlanTreeNodeDisplay instance
			nodeDisplay.itemData = value as XML;
		}
		
	}
}