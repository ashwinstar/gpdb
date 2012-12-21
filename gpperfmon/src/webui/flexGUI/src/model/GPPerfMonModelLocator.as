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
					health = new Health(this);
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
		
		// Releases all instance data (system, database, queries) 
		public static function releaseInstanceData():void
		{
			if (instance)
			{
				instance.system = null;
				instance.db = null;
				instance.queries = null;
				instance.health = null;
			}
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

		// Health class
		public var health:Health = null;
	}
}

/**
 * Inner class which restricts contructor access to Private
 */
class Private {}