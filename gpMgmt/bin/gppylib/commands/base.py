#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2008. All Rights Reserved. 
#
'''
base.py 

common base for the commands execution framework.  Units of work are defined as Operations
as found in other modules like unix.py.  These units of work are then packaged up and executed
within a GpCommand.  A GpCommand is just a common infrastructure for executing an Operation.

The general idea is that the application developer breaks the problem down into a set of 
GpCommands that need to be executed.  This class also provides a queue and set of workers
for executing this set of commands.  

                
'''

from Queue import  Queue,Empty
from threading import Thread

import subprocess
import logging
import os
import time
import unittest



from gppylib import gplog
from gppylib import gpsubprocess


logger=gplog.get_default_logger()



class WorkerPool:
    """TODO:"""
    
    def __init__(self,numWorkers=16,items=None):        
        self.workers=[]
        self.work_queue=Queue()
        self.completed_queue=Queue()
        
        if items is not None:
            for item in items:
                self.work_queue.put(item)
            
        for i in range(0,numWorkers):
            w = Worker("worker%d" % i,self)
            self.workers.append(w)
            w.start()
        
     
    
    ###
    def getNextWorkItem(self,timeout=None):
        return self.work_queue.get(block=True,timeout=timeout)
    
    def addFinishedWorkItem(self,command):
        self.completed_queue.put(command)    
        self.work_queue.task_done()
    
    
    def addCommand(self,cmd):   
        logger.debug("Adding cmd to work_queue: %s" % str(cmd.cmdStr)) 
        self.work_queue.put(cmd)
    
    
    def join(self):
        self.work_queue.join()
        return True


    def check_results(self):
        """ goes through all items in the completed_queue and throws an exception at the
            first one that didn't execute successfully
            
            throws ExecutionError
        """        
        try:
            while True:
                item=self.completed_queue.get(False)
                if not item.get_results().wasSuccessful():
                    raise  ExecutionError("",item)           
        except Empty:
            return      
            
        

    def isDone(self):
        raise "not implemented yet"
        if self.work_queue.empty() is False:
            return False
        
        
        #TODO:  check workers
        #for w in workers:
        
        return True
    
    def haltWork(self):
        logger.debug("WorkerPool haltWork()")
        for w in self.workers:
            w.haltWork()    



class Worker(Thread):
    """TODO:"""
    pool=None
    shouldStop=False
    cmd=None
    name=None
    
    def __init__(self,name,pool,timeout=5):
        self.name=name
        self.pool=pool
        self.timeout=timeout
        Thread.__init__(self)
        
    
    
    def run(self):
        """ 
            TODO:  ERROR HANDLING SILLY!
        """
        while True:
            try:
                logger.debug("[%s] try and get work from queue..." % self.name)
                self.cmd = self.pool.getNextWorkItem(timeout=self.timeout)
                
                if self.cmd is not None:
                    logger.debug("[%s] got cmd: %s" % (self.name,self.cmd.cmdStr))
                    #TODO:  catch Exception (especially ExecutionException)                        
                    self.cmd.run()
                    logger.debug("[%s] finished cmd" % self.name)
                    self.pool.addFinishedWorkItem(self.cmd)
                    self.cmd=None
                else:
                    if self.shouldStop:
                        logger.debug("[%s] stopping" % self.name)
                        return
                      
            except Empty:                
                if self.shouldStop:
                    logger.debug("[%s] stopping" % self.name)
                    return
                    
    
    def haltWork(self):
        logger.debug("[%s] haltWork" % self.name)
        self.shouldStop=True
        if self.cmd is not None:
            self.cmd.interrupt()
            self.cmd.cancel()
    
    def signalPassiveStop(self):
        self.shouldStop=True
        



"""
TODO: consider just having a single interface that needs to be implemented for
      describing work to allow the Workers to use it.  This would allow the user
      to better provide logic necessary.  i.e.  even though the user wants to 
      execute a unix command... how the results are interpretted are highly
      application specific.   So we should have a separate level of abstraction
      for executing UnixCommands and DatabaseCommands from this one.
      
      other things to think about:
      -- how to support cancel
      -- how to support progress
      -- undo?
      -- blocking vs. unblocking
      
"""

        
        
   
        
   



#--------------------------------NEW WORLD-----------------------------------

class CommandResult():
    """ Used as a way to package up the results from a GpCommand
    
    """
    
    #rc,stdout,stderr,completed,halt
    
    def __init__(self,rc,stdout,stderr,completed,halt):
        self.rc=rc
        self.stdout=stdout
        self.stderr=stderr
        self.completed=completed
        self.halt=halt
        pass

    def printResult(self):
        res = "cmd had rc=%d completed=%s halted=%s\n  stdout='%s'\n  " \
            "stderr='%s'" % (self.rc,str(self.completed), str(self.halt), self.stdout, self.stderr)
        return res
    
    def wasSuccessful(self):
        if self.halt:
            return False
        if not self.completed:
            return False
        if self.rc is not 0:
            return False
        return True
        
    



class ExecutionError(Exception): 
    def __init__(self,summary,cmd):
        self.summary=summary
        self.cmd=cmd
    def __str__(self):
        #TODO: improve dumping of self.cmd
        return "ExecutionError: '%s' occured.  Details: '%s'  %s" %\
             (self.summary,self.cmd.cmdStr,self.cmd.get_results().printResult())



#specify types of execution contexts.
LOCAL=1
REMOTE=2
RMI=3

        
def createExecutionContext(execution_context_id,remoteHost):
        if execution_context_id == LOCAL:
            return LocalExecutionContext()
        elif execution_context_id == REMOTE:
            if remoteHost is None:
                raise Exception("Programmer Error.  Specified REMOTE execution context but didn't provide a remoteHost")
            return RemoteExecutionContext(remoteHost)
        elif execution_context+id == RMI:
            return RMIExecutionContext()
        
        
class ExecutionContext():
    """ An ExecutionContext defines where and how to exeucte the Command and how to 
    gather up information that are the results of the command.  
    
    """
    def __init__(self):
        pass
    
    def execute(self,cmd):
        pass

    def interrupt(self):
        pass
    
    def cancel(self):
        pass
    

class LocalExecutionContext(ExecutionContext):
    proc=None
    halt=False
    completed=False
    
    def __init__(self):
        pass
    
    def execute(self,cmd):
        self.proc = gpsubprocess.Popen(cmd.cmdStr, env=None, shell=True, stderr=subprocess.PIPE, 
                                       stdout=subprocess.PIPE, close_fds=True)
        
        (rc,stdout_value,stderr_value)=self.proc.communicate2()
        self.completed=True
         
        cmd.set_results(CommandResult(rc,"".join(stdout_value),"".join(stderr_value),self.completed,self.halt))

    def cancel(self):
        if self.proc:
            os.kill(self.proc.pid, signal.SIGTERM)

    def interrupt(self):
        self.halt=True
        if self.proc:
            self.proc.cancel()
    
    
    
class RemoteExecutionContext(LocalExecutionContext):
    
    def __init__(self,targetHost):
        self.targetHost=targetHost
        pass
    
    def execute(self,cmd):
        cmd.cmdStr="ssh  %s \"%s\"" % (self.targetHost,cmd.cmdStr)
        LocalExecutionContext.execute(self,cmd)
        pass
      


class RMIExecutionContext(ExecutionContext):
    """ Leave this as a big old TODO: for now.  see agent.py for some more details"""
    def __init__(self):
        pass



def ping_list(pingName,host_list,queue):
                
        logger.debug("pingHosts start")
        for host in host_list:
            ping_cmd=Ping("ping",host,ctxt=LOCAL,remoteHost=None)
            queue.addCommand(ping_cmd)
        queue.join()  
        queue.check_results()      
        logger.debug("pingHosts finished")



# Helper commands for running commands:

def run_list(queue,host_list,cmdGenerator,shouldValidate=True):
    for cmd in cmdGenerator(host_list):
        queue.addCommand(cmd)
    queue.join()
    queue.check_results()

    


class Command:
    """ TODO: 
    """
    name=None
    cmdStr=None
    results=None
    exec_context=None
    
    
    def __init__(self,name,cmdStr,ctxt=LOCAL,remoteHost=None):
        self.name=name
        self.cmdStr=cmdStr        
        self.exec_context=createExecutionContext(ctxt,remoteHost)
        self.remoteHost=remoteHost
        

    def run(self,validateAfter=False):
        self.exec_context.execute(self)
        
        if validateAfter:
            self.validate()
        pass
    
    
    
    def set_results(self,results):
        self.results=results
    
        
    def get_results(self):
        return self.results
    
    
    def cancel(self):
        self.exec_context.cancel()    
    
    
    def was_successful(self):
        if self.results is None:
            return False
        else:
            return self.results.wasSuccessful()
    
        
    def validate(self):
        """Plain vanilla validation which expects a 0 return code."""        
        if self.results.rc != 0:
            raise ExecutionError("non-zero rc: %d" % self.results.rc, self)            
        
    
