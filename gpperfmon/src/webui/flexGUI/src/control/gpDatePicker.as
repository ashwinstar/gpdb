package control
{
	import flash.events.Event;
	import flash.events.MouseEvent;
	import flash.geom.Point;
	
	import mx.containers.ControlBar;
	import mx.containers.HBox;
	import mx.containers.VBox;
	import mx.controls.Button;
	import mx.controls.DateChooser;
	import mx.controls.DateField;
	import mx.controls.Image;
	import mx.controls.Label;
	import mx.controls.NumericStepper;
	import mx.controls.Text;
	import mx.events.FlexEvent;
	import mx.events.FlexMouseEvent;
	import mx.managers.PopUpManager;

	public class gpDatePicker extends VBox
	{
		private var dtInput:Label;
		private var dtPick:DateChooser;
		private var dtHour:NumericStepper;
		private var dtMin:NumericStepper;
		private var dtSec:NumericStepper;
		
		private var dtSelectedDateTime:Number = -1;
		
		private var dtPop:Button;	
		private var dtClear:Button;	
		private var dtTemp:DateField;
		private var dtPanel:ControlBar;
		private var pickPanel:HBox;
		private var pickPanel2:VBox;
		
		private var buttonPanel:HBox;
		private var okButton:Button;
		private var cancelButton:Button;
		
		private var lHour:Label;
		private var lMin:Text;
		private var lSec:Label;
		
		private var _yearNavigationEnabled:Boolean;
		
		private var _addedToPopupManager:Boolean = false;
		private var _showingDTPick:Boolean = false;
		
		private var _minDate:Date;
		private var _maxDate:Date;

		[Embed(source='../assets/img/cal2.jpg')]
   		private var calendarIcon:Class;	
   		
		[Embed(source='../assets/img/cross.png')]
   		private var crossIcon:Class;	

	/**
	 * Event to trigger notification of date change
	 */
	[Event(name="changeDate", type="flash.events.Event")]
	
	
	
	
	
	/**
	 * @private
	 */
	private var _iconside:String = new String();
	
	[Bindable("iconSide")]
	[Inspectable(defaultValue="right")]
	
	public function set iconSide(ss:String):void
	{
		this._iconside=ss;
	}
	public function get iconSides():String
	{
		return this._iconside;
	}

	/**
	 * @private
	 */
	private var _date:Date = new Date();
	
	[Bindable("datetime")]
	[Inspectable()]
	
	public function set gpDateTime(ss:String):void
	{
		var tmp:String;
		this._date = new Date(Date.parse(ss));
		dtInput.text='';

		dtPick.selectedDate = this._date;

		if ((_date.getMonth()+1)<10)
		{
			tmp = "0".concat(_date.getMonth()+1);
		}
		else
		{
			tmp = "".concat(_date.getMonth()+1);
		}
		dtInput.text = dtInput.text.concat(tmp,"/");
			
		if (_date.getDate()<10)
		{
			tmp = "0".concat(_date.getDate());
		}
		else
		{
			tmp = "".concat(_date.getDate());
		}
		dtInput.text = dtInput.text.concat(tmp,"/");

		dtInput.text = dtInput.text.concat(_date.getFullYear()," ");
		
		if (dtHour.value<10)
		{
			tmp = "0".concat(dtHour.value);
		}
		else
		{
			tmp = "".concat(dtHour.value);
		}
		dtInput.text = dtInput.text.concat(tmp,":");
		
		if (dtMin.value<10)
		{
			tmp = "0".concat(dtMin.value);
		}
		else
		{
			tmp = "".concat(dtMin.value);
		}
		dtInput.text = dtInput.text.concat(tmp,":");

		if (dtSec.value<10)
		{
			tmp = "0".concat(dtSec.value);
		}
		else
		{
			tmp = "".concat(dtSec.value);
		}
		dtInput.text = dtInput.text.concat(tmp);
	}

	public function get gpDateTime():String
	{
		return this._date.toDateString();
	}

	public function get gpDate():Date
	{
		return this._date;
	}	

	public function get getSelectedDateTime():Number
	{
		return this.dtSelectedDateTime;
	}	
	
	public function get getFormattedDateTime():String
	{
		return this.dtInput.text;
	}

	public function set minDate(dd:Date):void
	{
		this._minDate = dd;
	}
	
	public function get minDate():Date
	{
		return this._minDate;
	}
	
	public function set maxDate(dd:Date):void
	{
		this._maxDate = dd;
	}
	
	public function get maxDate():Date
	{
		return this._maxDate;
	}
	
    /**
     *  @private
     */
    private var yearNavigationEnabledChanged:Boolean = false;
    
    [Bindable("yearNavigationEnabledChanged")]
    [Inspectable(defaultValue="false")]

    /**
     * @private
     */
     private var _gpCalAlpha:Number = 0.9;
      
	[Bindable("gpCalAlpha")]
	[Inspectable(defaultValue=".9")]

	public function set gpCalAlpha(num:Number):void
	{
		this._gpCalAlpha = num;
	}
	
	public function get gpCalAlpha():Number
	{
		return this._gpCalAlpha;
	}

    /**
     *  Enables year navigation. When <code>true</code>
     *  an up and down button appear to the right
     *  of the displayed year. You can use these buttons
     *  to change the current year.
     *  These button appear to the left of the year in locales where year comes 
     *  before the month in the date format.
     *
     *  @default false
     */
    public function get yearNavigationEnabled():Boolean
    {
        return _yearNavigationEnabled;
    }

    /**
     *  @private
     */
    public function set yearNavigationEnabled(value:Boolean):void
    {
        _yearNavigationEnabled = value;
        yearNavigationEnabledChanged = true;

        invalidateProperties();
    }
		
		
		public function gpDatePicker():void
		{
			height = 25;
			width=220;
			
            addEventListener(FlexEvent.INITIALIZE, initializeHandler);

			dtPanel = new ControlBar();
			pickPanel = new HBox();
			pickPanel2 = new VBox();
			
			dtInput = new Label();
			dtInput.text = "[not specified]";
			dtHour = new NumericStepper();
			dtMin = new NumericStepper();
			dtSec = new NumericStepper();
			
			dtPop = new Button();
			dtClear = new Button();
			
			lHour = new Label();
			lMin = new Text();
			lSec = new Label();

			buttonPanel = new HBox();
            buttonPanel.setStyle ("paddingLeft",5);
            buttonPanel.setStyle ("paddingRight", 5);
            buttonPanel.setStyle ("paddingTop", 5);
            buttonPanel.setStyle ("paddingBottom", 5);
            buttonPanel.setStyle ("horizontalGap", 5);
            buttonPanel.setStyle ("horizontalAlign", "center");
			okButton = new Button();
			okButton.label = "Ok";
			okButton.width = 80;
			okButton.addEventListener(MouseEvent.CLICK,icalClick);			
			cancelButton = new Button();
			cancelButton.label = "Cancel";
			cancelButton.width = 80;
			cancelButton.addEventListener(MouseEvent.CLICK,icalCancelClick);		

			dtInput.width=150;
			dtHour.width=45;
			dtHour.maximum=23;
			dtMin.width=45;
			dtMin.maximum=59;
			dtSec.width=45;
			dtSec.maximum=59;
			
/* 			lMin.htmlText = "<b>Time:</b>";
			lMin.setStyle("backgroundColor",0xFFFFFF);
			lMin.alpha=_gpCalAlpha;
			lMin.stylesInitialized();
*/
            dtPanel.setStyle ("paddingLeft",0);
            dtPanel.setStyle ("paddingRight", 0);
            dtPanel.setStyle ("paddingTop", 0);
            dtPanel.setStyle ("paddingBottom", 0);
            dtPanel.height=25;
            
			dtPop.setStyle ("upIcon", calendarIcon); 
			dtPop.setStyle ("overIcon", calendarIcon);
			dtPop.setStyle ("downIcon", calendarIcon);  
			dtPop.width=20;
			dtPop.height=20;
			dtPop.addEventListener(MouseEvent.CLICK,ibtnClick);

			dtClear.setStyle ("upIcon", crossIcon); 
			dtClear.setStyle ("overIcon", crossIcon);
			dtClear.setStyle ("downIcon", crossIcon);
			dtClear.width=20;
			dtClear.height=20;
			dtClear.addEventListener(MouseEvent.CLICK,clearClick);

			dtPick = new DateChooser();
			dtPick.width = 180;
			dtPick.visible=true;
			dtPick.isPopUp=true;
			dtPick.yearNavigationEnabled = yearNavigationEnabled;
			dtPick.alpha=_gpCalAlpha;
			//dtPick.addEventListener(MouseEvent.CLICK,icalClick);
			pickPanel2.addEventListener(FlexMouseEvent.MOUSE_DOWN_OUTSIDE,icalClick);
			
			lHour.htmlText = "<b>Time:</b>";
			lHour.setStyle("verticalAlign","middle");
			

			pickPanel.setStyle("paddingBottom",0);
			pickPanel.setStyle("paddingTop",0);
			pickPanel.setStyle("paddingLeft",0);
			pickPanel.setStyle("paddingRight",0);
			pickPanel.setStyle("horizontalGap",1);
			pickPanel.setStyle("horizontalAlign","right");

			pickPanel2.setStyle("backgroundColor",0xFFFFFF);
			pickPanel2.setStyle("paddingTop",10);
			pickPanel2.setStyle("paddingLeft",10);
			pickPanel2.setStyle("paddingRight",10);
			pickPanel2.setStyle("paddingBottom",10);
			pickPanel2.setStyle("cornerRadius",10);
			pickPanel2.setStyle("borderColor",dtPick.getStyle("borderColor"));
			pickPanel2.setStyle("borderThickness",1);
			pickPanel2.setStyle("borderStyle","solid");
			
			
			pickPanel.percentWidth=100;
			
			pickPanel.setStyle("cornerRadius",100);
			pickPanel2.alpha=_gpCalAlpha;
			pickPanel2.setStyle("verticalGap",0);
			
			dtPick.setStyle("cornerRadius",10);
			dtPick.setStyle("paddingTop",0);
			dtPick.alpha=_gpCalAlpha;
			dtPick.yearNavigationEnabled=true;      
		}

		private function captureDateTime():void
		{
			this._date = dtPick.selectedDate;
			_date.setHours(dtHour.value,dtMin.value,dtSec.value,0);
			
			dtSelectedDateTime = _date.getTime();
			
			dtInput.text = "";
			var tmp:String;

			if ((_date.getMonth()+1)<10)
			{
				tmp = "0".concat(_date.getMonth()+1);
			}
			else
			{
				tmp = "".concat(_date.getMonth()+1);
			}
			dtInput.text = dtInput.text.concat(tmp,"/");
			
			if ((_date.getDate())<10)
			{
				tmp = "0".concat(_date.getDate());
			}
			else
			{
				tmp = "".concat(_date.getDate());
			}
			dtInput.text = dtInput.text.concat(tmp,"/");

			dtInput.text = dtInput.text.concat(_date.getFullYear()," ");
			
			if (dtHour.value<10)
			{
				tmp = "0".concat(dtHour.value);
			}
			else
			{
				tmp = "".concat(dtHour.value);
			}
			dtInput.text = dtInput.text.concat(tmp,":");
			
			if (dtMin.value<10)
			{
				tmp = "0".concat(dtMin.value);
			}
			else
			{
				tmp = "".concat(dtMin.value);
			}
			dtInput.text = dtInput.text.concat(tmp,":");

			if (dtSec.value<10)
			{
				tmp = "0".concat(dtSec.value);
			}
			else
			{
				tmp = "".concat(dtSec.value);
			}
			dtInput.text = dtInput.text.concat(tmp);
			
			removePopup();
			dispatchEvent(new Event("changeDate",false,true));
		}
		
		private function removePopup():void
		{
			dtPop.drawFocus(false);
			pickPanel2.visible=false;
			_showingDTPick=false;			
		}
		
		private function icalClick(event:MouseEvent):void
		{
			if (pickPanel2.visible)
				captureDateTime();
		}
		
		private function icalCancelClick(event:MouseEvent):void
		{
			if (pickPanel2.visible)
				removePopup();
		}

		private function initializeHandler(event:FlexEvent):void
		{
			init();
		}		
		
		private function ibtnClick(event:MouseEvent):void
		{
			if (!_addedToPopupManager)
			{
				PopUpManager.addPopUp(pickPanel2,this,false);
				_addedToPopupManager=true;
			}
			if (!_showingDTPick)
			{
				PopUpManager.bringToFront(pickPanel2);
				_showingDTPick = true;
				// adjust the position so that it doesn't lay over the input box
				var dtw:int = dtPop.width;
				var dth:int = dtPop.height;
				var usw:int = this.unscaledWidth;
				
				if (_iconside.toLowerCase()=="left")
				{
					usw = usw-170;
				}
				var point:Point = new Point(usw - dtw,dth);
        		point = localToGlobal(point);
        		pickPanel2.move(point.x,point.y);
        		dtPop.drawFocus(true);
				pickPanel2.visible=true;
			}
			else
			{
				dtPop.drawFocus(false);
				pickPanel2.visible=false;
				_showingDTPick=false;
			}			
				
		}
		
		private function clearClick(event:MouseEvent):void
		{
			dtInput.text = "[not specified]";
			dtSelectedDateTime = -1;
		}
				
		private function init():void
		{
			dtPick.alpha=_gpCalAlpha;
			pickPanel.alpha=_gpCalAlpha;
			pickPanel2.alpha=_gpCalAlpha;
			
			dtPick.selectedDate = _date;
			dtHour.value = _date.getHours();
			dtMin.value = _date.getMinutes();
			dtSec.value = _date.getSeconds();
			
			if (_iconside.toLowerCase()=="left")
			{
				dtInput.setStyle("textAlign","left");
				dtPanel.addChild(dtPop);
				dtPanel.addChild(dtClear);
				dtPanel.addChild(dtInput);
			}
			else
			{
				dtInput.setStyle("textAlign","right");
				dtPanel.addChild(dtInput);
				dtPanel.addChild(dtPop);
				dtPanel.addChild(dtClear);
			}
			

			pickPanel.addChild(lHour);
			pickPanel.addChild(dtHour);
			pickPanel.addChild(dtMin);
			pickPanel.addChild(dtSec);
			
			pickPanel2.addChild(pickPanel);
			pickPanel2.addChild(dtPick);
			
			buttonPanel.addChild(okButton);
			buttonPanel.addChild(cancelButton);
			
			pickPanel2.addChild(buttonPanel);

			addChild(dtPanel);
								
		}
	}
}