package model
{
	[Bindable]
	public class GPPerfMonModelLocator
	{
		/**
		 * Singleton instance of GPPerfMonModelLocator
		 */
		public static var instance:GPPerfMonModelLocator;
		
		public function GPPerfMonModelLocator(access:Private)
		{
			if (access != null)
			{
				if (instance == null)
				{
					instance = this;
					
					// Do initialization
					system = new System(this);
					db = new Database(this);
					queries = new Queries(this);
				}
			}
			else
			{
				throw new Error("Singleton Exception", "GPPerfMonModelLocator");
			}
		}
		
		/**
		 * Returns the Singleton instance of GPPerfMonModelLocator
		 */
		public static function getInstance() : GPPerfMonModelLocator
		{
			if (instance == null)
			{
				instance = new GPPerfMonModelLocator( new Private );
			}
			return instance;
		}			
		
		// NavState class
		public var nav:NavState = new NavState;
		
		// Session class
		public var session:Session = new Session;	
		
		// System class
		public var system:System = null;

		// Database class
		public var db:Database = null;
		
		// Queries class
		public var queries:Queries = null;
	}
}

/**
 * Inner class which restricts contructor access to Private
 */
class Private {}