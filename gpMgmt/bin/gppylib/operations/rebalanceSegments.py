from gppylib.gparray import GpArray
from gppylib.db import dbconn
from gppylib.commands.gp import GpSegStopCmd, GpRecoverseg
from gppylib.commands.base import WorkerPool, REMOTE
from gppylib import gplog

logger = gplog.get_default_logger()

class GpSegmentRebalanceOperation:
    def __init__(self, gpEnv, gpArray):
        self.gpEnv = gpEnv
        self.gpArray = gpArray
    
    def rebalance(self):
        # Get the unbalanced primary segments grouped by hostname
        # These segments are what we will shutdown.
        logger.info("Getting unbalanced segments")
        unbalanced_primary_segs = GpArray.getSegmentsByHostName(self.gpArray.get_unbalanced_primary_segdbs())
        pool = WorkerPool()
        
        count = 0
        logger.info("Stopping unbalanced primary segments...")
        for hostname in unbalanced_primary_segs.keys():
            cmd = GpSegStopCmd("stop unbalanced primary segs",
                               self.gpEnv.getGpHome(),
                               self.gpEnv.getGpVersion(),
                               'fast',
                               unbalanced_primary_segs[hostname],
                               ctxt=REMOTE,
                               remoteHost=hostname,
                               timeout=600)
            pool.addCommand(cmd)
            count+=1
            
        pool.wait_and_printdots(count, False)
        
        failed_count = 0
        completed = pool.getCompletedItems()
        for res in completed:
            if not res.get_results().wasSuccessful():
                failed_count+=1
                
        if failed_count > 0:
            logger.warn("%d segments failed to stop.  A full rebalance of the")
            logger.warn("system is not possible at this time.  Please check the")
            logger.warn("log files, correct the problem, and run gprecoverseg -r")
            logger.warn("again.")
            logger.info("gprecoverseg will continue with a partial rebalance.")
        
        pool.empty_completed_items()
        # issue a distributed query to make sure we pick up the fault
        # that we just caused by shutting down segments
        try:
            logger.info("Triggering segment reconfiguration")
            dburl = dbconn.DbURL()
            conn = dbconn.connect(dburl)
            dbconn.execSQL(conn, "SELECT * from gp_dist_random('gp_id')")
        except Exception, ex:
            # This exception is expected
            pass

        # Final step is to issue a recoverseg operation to resync segments
        logger.info("Starting segment synchronization")
        cmd = GpRecoverseg("rebalance recoverseg")
        pool.addCommand(cmd)
        pool.wait_and_printdots(1, False)
        
        # check that recoverseg was successful
        completed = pool.getCompletedItems()
        if not completed[0].get_results().wasSuccessful():
            logger.error("Failed to start the synchronization step of the segment rebalance.")
            logger.error("Check the gprecoverseg log file, correct any problems, and re-run")
            logger.error("'gprecoverseg -a'.")        
            logger.error(completed[0].get_results())
            raise Exception("Error synchronizing.")
            
        
        
