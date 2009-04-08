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
from pygresql.pg import DB


logger=gplog.get_default_logger()



class WorkerPool:
    """TODO:"""
    
    def __init__(self,numWorkers=16,items=None):        
        self.workers=[]
        self.work_queue=Queue()
        self.completed_queue=Queue()
        self.num_assigned=0
        if items is not None:
            for item in items:                
                self.work_queue.put(item)
                self.num_assigned += 1
            
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
        self.num_assigned += 1
    
    
    def join(self):
        self.work_queue.join()
        return True
    
    def joinWorkers(self):
        for w in self.workers:
            w.join()

    def getCompletedItems(self):
        list=[]
        try:
            while True:
                item=self.completed_queue.get(False)
                if item is not None:
                    list.append(item)                           
        except Empty:
            return list
        return list  #just to be sure
    

    def check_results(self):
        """ goes through all items in the completed_queue and throws an exception at the
            first one that didn't execute successfully
            
            throws ExecutionError
        """        
        try:
            while True:
                item=self.completed_queue.get(False)
                if not item.get_results().wasSuccessful():
                    raise  ExecutionError("Error Executing Command: ",item)           
        except Empty:
            return      
    
    def empty_completed_items(self):
        while not self.completed_queue.empty():
            self.completed_queue.get(False)         
        

    def isDone(self):
        #TODO: not sure that qsize() is safe
        return (self.num_assigned == self.completed_queue.qsize())
        
    
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
        try_count = 0
        while True:
            try:
                if try_count == 5:
                    logger.debug("[%s] try and get work from queue..." % self.name)
                    try_count = 0
                
                if self.shouldStop:
                    logger.debug('[%s] stopping' % self.name)
                    return
                    
                self.cmd = self.pool.getNextWorkItem(timeout=self.timeout)
                
                if self.cmd is not None and not self.shouldStop:
                    logger.debug("[%s] got cmd: %s" % (self.name,self.cmd.cmdStr))
                    #TODO:  catch Exception (especially ExecutionException)                        
                    self.cmd.run()
                    logger.debug("[%s] finished cmd: %s" % (self.name, self.cmd))
                    self.pool.addFinishedWorkItem(self.cmd)
                    self.cmd=None
                    try_count = 0
                else:
                    try_count += 1
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

    def __str__(self):
        return self.printResult()
        
    



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

        
def createExecutionContext(execution_context_id,remoteHost,stdin):
        if execution_context_id == LOCAL:
            return LocalExecutionContext(stdin)
        elif execution_context_id == REMOTE:
            if remoteHost is None:
                raise Exception("Programmer Error.  Specified REMOTE execution context but didn't provide a remoteHost")
            return RemoteExecutionContext(remoteHost,stdin)
        elif execution_context_id == RMI:
            return RMIExecutionContext()
        
        
class ExecutionContext():
    """ An ExecutionContext defines where and how to execute the Command and how to 
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
    
    def __init__(self,stdin):
        self.stdin = stdin
        pass
    
    def execute(self,cmd):
        
        # executable='/bin/bash' is to ensure the shell is bash.  bash isn't the
        # actual command executed, but the shell that command string runs under.
        self.proc = gpsubprocess.Popen(cmd.cmdStr, env=None, shell=True, 
                                       executable='/bin/bash',
                                       stdin=subprocess.PIPE,
                                       stderr=subprocess.PIPE, 
                                       stdout=subprocess.PIPE, close_fds=True)
        
        (rc,stdout_value,stderr_value)=self.proc.communicate2(input=self.stdin)
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
    
    def __init__(self,targetHost,stdin):
        self.targetHost=targetHost
        self.stdin = stdin
        pass
    
    def execute(self,cmd):
        # Escape " for remote execution otherwise it interferes with ssh
        cmd.cmdStr = cmd.cmdStr.replace('"', '\\"')
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
    
    
    def __init__(self,name,cmdStr,ctxt=LOCAL,remoteHost=None,stdin=None):
        self.name=name
        self.cmdStr=cmdStr        
        self.exec_context=createExecutionContext(ctxt,remoteHost,stdin=stdin)
        self.remoteHost=remoteHost
        
    def __str__(self):
        if self.results:
            return "%s cmdStr='%s'  had result: %s" % (self.name,self.cmdStr,self.results)
        else:
            return "%s cmdStr='%s'" % (self.name,self.cmdStr)
        

    def run(self,validateAfter=False):
        faultPoint = os.getenv('GP_COMMAND_FAULT_POINT')
        if not faultPoint or (self.name and not self.name.startswith(faultPoint)):
            self.exec_context.execute(self)
        else:
            # simulate error
            self.results = CommandResult(1,'Fault Injection','Fault Injection' ,False,True)
        
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
        

class SQLCommand(Command):
    
    def __init__(self,name):
        self.name=name
        self.conn = None

    
    def run(self,validateAfter=False):
        raise ExecutionError("programmer error.  implementors of SQLCommand must implement run()")
    
    def set_results(self,results):
        raise ExecutionError("TODO:  must implement")
    
    def get_results(self):
        raise ExecutionError("TODO:  must implement")
    
    def interrupt(self):
        # No execution context for SQLCommands
        pass
    
    def cancel(self):
        # if self.conn is not set we cannot cancel.
        if self.conn:
            DB(self.conn).cancel()
        else:
            raise ExecutionError("programmer error.  must set self.conn before calling cancel")

    
    def was_successful(self):
        raise ExecutionError("TODO:  must implement")
    
    def validate(self):
        raise ExecutionError("TODO:  must implement")
    
    
