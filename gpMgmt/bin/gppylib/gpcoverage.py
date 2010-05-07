"""
This file is a wrapper around figleaf and will start/stop coverage as
needed.  It also includes a method for generating the HTML reports.
"""

import os
import figleaf
from gppylib.commands.base import Command, LOCAL, REMOTE
from gppylib.commands.unix import RemoveFiles
from gppylib.gplog import *


#------------------------------------------------------------------------------
class GpWriteFigleafCoverageHtml(Command):
    """Command to write out figleaf html reports to disk based on the
    coverage information that has been collected."""
    
    def __init__(self,name,filename, directory,ctxt=LOCAL,remoteHost=None):
        gphome = os.getenv("GPHOME", None)
        if not gphome:
            raise Exception('GPHOME environment variable not set.')
        cmdStr = "%s -d %s %s" % (os.path.normpath(gphome + '/lib/python/figleaf/figleaf2html'), directory, filename)        
        Command.__init__(self,name,cmdStr,ctxt,remoteHost)

    @staticmethod
    def local(name, coverfile, directory):
        cmd = GpWriteFigleafCoverageHtml(name, coverfile, directory)
        cmd.run(validateAfter=True)

    
#------------------------------------------------------------------------------
class GpFigleafCoverage():
    """Figleaf code coverage class.  Currently this is what we use for
    python code coverage."""
    
    def __init__(self, filename=None):
        """filename is the file to write coverage information to."""
        self.logger = get_default_logger()
        
        try:
            if not filename:
                self.filename = os.path.normpath(os.path.expanduser("~") + '/.figleaf/cover.out')
            else:
                self.filename=filename
            self.running = False
            self.logger.debug('Code coverage file name: %s' % self.filename)
        except Exception, e:
            self.logger.error('Error initializing code coverage: %s' % e)
        
    def start(self):
        """Starts coverage collection if the environment variable 
        USE_FIGLEAF is set."""
        try:
            if os.getenv('USE_FIGLEAF', None):
                self.logger.info('Code coverage will be generated')
                dir = os.path.dirname(self.filename)
                if not os.path.exists(dir):
                    os.mkdir(dir)
                self.running = True
                figleaf.start()
        except Exception, e:
            self.logger.error('Error starting code coverage: %s' % e)

    def stop(self):
        """Stops code coverage."""
        try:
            if self.running:
                self.logger.info('Stopping code coverage')
                figleaf.stop()
                figleaf.write_coverage(self.filename)
                self.running = False
        except Exception, e:
            self.logger.error('Error stopping code coverage: %s' % e)
        
    def generate_report(self, directory=None):
        """Generates the html reports and puts them in the directory
        specified."""
        if os.getenv('USE_FIGLEAF', None):
            try:
                if not directory:
                    directory = os.path.normpath(os.path.expanduser("~") + '/.figleaf')
                self.logger.info('Generating code coverage HTML reports to %s' % directory)
                GpWriteFigleafCoverageHtml.local('Generate HTML', self.filename, directory)
            except Exception, e:
                self.logger.error('Error generating HTML code cover reports: %s' % e)
                
    def delete_files(self, directory=None):
        """Deletes code coverage files."""
        if os.getenv('USE_FIGLEAF', None):
            self.logger.info('Deleting coverage files...')
            try:
                RemoveFiles.local('Remove coverage file', self.filename)
                if not directory:
                    directory = os.path.normpath(os.path.dirname(self.filename))
                RemoveFiles.local('Remove html files', directory + '/*.html')
            except:
                self.logger.error('Failed to clean up coverage files')
        
                
                
# The coverage tool to use
#if os.getenv('USE_FIGLEAF', None):
GP_COVERAGE_CLASS=GpFigleafCoverage
#else:
#    GP_COVERAGE_CLASS=<some other coverage class>


#------------------------------------------------------------------------------
class GpCoverage(GP_COVERAGE_CLASS):
    """Class the controls code coverage.  Right now this inherits from
    GpFigleafCoverage, but in the future we may find a better code coverage
    tool and switch to that.  With this class, we can do that without 
    touching any of the management utilities or modules."""
    pass
