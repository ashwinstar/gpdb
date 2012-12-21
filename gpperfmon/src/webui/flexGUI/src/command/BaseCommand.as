package command
{
	import command.ICommand;
	
	public class BaseCommand
	{
		public function chainCommand(next:ICommand):void 
		{
			nextCommand = next;
		}
		
		protected function executeChainedCommand():void
		{
			if (nextCommand)
				nextCommand.execute();
		}
		
		private var nextCommand:ICommand = null;
	}
}
