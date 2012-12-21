package control {
     import mx.controls.Tree;
     import mx.core.ScrollPolicy;
     
     public class AutoSizeTree extends Tree
     {
     	  public var contentsWidth:Number = NaN;
     	
          public function AutoSizeTree(){
               super();
               horizontalScrollPolicy = ScrollPolicy.AUTO;
          }
          
          override protected function updateDisplayList(unscaledWidth:Number, unscaledHeight:Number):void
          {
          	  if (isNaN(contentsWidth))
          	  	maxHorizontalScrollPosition = NaN;
          	  else
          	  {
              		var diffWidth:Number = contentsWidth - (unscaledWidth - viewMetrics.left - viewMetrics.right);
               
               		if (diffWidth <= 0)
                    	maxHorizontalScrollPosition = NaN;
               		else
                    	maxHorizontalScrollPosition = diffWidth;
          	  }
                    
               super.updateDisplayList(unscaledWidth, unscaledHeight);
          }
     }
}
