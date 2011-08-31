#!/usr/bin/env python 
#
# Line too long - pylint: disable=C0301
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST

import tarfile
import glob
import shutil
import os, sys
import yaml     # this sits in gpMgmt/bin/ext/yaml 
import platform

try:
    from yaml.scanner import ScannerError
    from gppylib.commands import gp
    from gppylib.commands.unix import Scp
    from gppylib.gpversion import GpVersion
    from Queue import Queue
    from gppylib import gplog
    from gppylib.commands.base import Command, REMOTE, WorkerPool, ExecutionError
    from gppylib.operations import Operation
    from gppylib.operations.unix import RemoveRemoteTree, RemoveRemoteFile, CheckRemoteDir, MakeRemoteDir 
    from gppylib.mainUtils import ExceptionNoStackTraceNeeded
except ImportError, ex:
    sys.exit('Operation: Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(ex)) 

logger = gplog.get_default_logger()

GPHOME = gp.get_gphome() 

SPECFILE_NAME = 'gppkg_spec.yml'
SPECFILE_REQUIRED_TAGS = ['pkgname', 'version', 'architecture', 'os', 'description', 'gpdbmin', 'gpdbmax']
SPECFILE_OPTIONAL_TAGS = ['preinstall', 'postinstall', 'preuninstall', 'postuninstall']

RPM_DATABASE = os.path.join(GPHOME, 'addons/.gprpmdb')
RPM_INSTALLATION_PATH = GPHOME
GPPKG_ARCHIVE_PATH = os.path.join(GPHOME, 'addons/archive')

TEMP_EXTRACTION_PATH = GPHOME + '/.tmp'
DEPS_DIR = 'deps'

REMOTE_COMMAND_PREFIX = 'cd ' + TEMP_EXTRACTION_PATH + ';'

class GpdbVersionError(Exception):
    
    '''
        Exception to notify that the gpdb version
        does not match
    '''
    pass

class AlreadyInstalledError(Exception):
    '''
        Exception to notify that the package
        was already installed. 
        This exception is used during install  
    '''
    def __init__(self, pkgname):
        Exception.__init__(self, '%s is Already Installed' % pkgname)

class NotInstalledError(Exception):
    '''
        Exception to notify that the package
        was not installed. 
        This exception is used during uninstall  
    '''
    def __init__(self, pkgname):
        Exception.__init__(self, '%s is Not Installed' % pkgname)

class BuildPkgError(Exception):
    '''
        Exception to notify that there was an error during
        the building of a gppkg
    '''
    pass

class MissingDependencyError(Exception):
    '''
        Exception to catch missing dependency
    '''
 
    def __init__(self, value):
        Exception.__init__(self, 'Dependency %s is missing' % value )

class OSCompatibilityError(Exception):
    '''
        Exception to notify that OS does not meet the 
        requirement
    '''
    def __init__(self, requiredos, foundos):
        Exception.__init__(self, '%s OS required. %s OS found' % (requiredos, foundos))

class ArchCompatibilityError(Exception):
    '''
        Exception to notify that architecture does not meet
        the requirement
    '''
    def __init__(self, requiredarch, foundarch):
        Exception.__init__(self, '%s Arch required. %s Arch found' % (requiredarch, foundarch))

class RequiredDependencyError(Exception):
    '''
        Exception to notify that the package being uninstalled
        is a dependency for another package
    '''
    pass

class LocalCommand(Operation):
    '''
        Executes a command locally
    '''

    def __init__(self, cmd_str, echo = False):
        self.cmd_str = cmd_str
        self.echo = echo

    def execute(self):

        logger.debug(self.cmd_str)
        cmd = Command(name = 'LocalCommand', cmdStr = self.cmd_str)
        cmd.run(validateAfter = True)
        if self.echo:
            echo_str = cmd.get_results().stdout.strip()
            if echo_str:
                logger.info(echo_str)
        return cmd.get_results()

class RemoteCommand(Operation):
    '''
        Executes a command on a remote host
    '''

    def __init__(self, cmd_str, host_list):
        self.cmd_str = cmd_str
        self.host_list = host_list
       
    def execute(self):
       
        logger.debug(self.cmd_str)

        # Create Worker pool
        # and add commands to it
        pool = WorkerPool()

        for host in self.host_list:
            #When we run a command, the command gets executed in the users home directory
            #But in our case, all the required files will be in $GPHOME/.tmp so we need to 'cd' 
            #to that directory each time before running any command
            cmd = Command(name = 'Remote Command', cmdStr = REMOTE_COMMAND_PREFIX + self.cmd_str, ctxt = REMOTE, remoteHost = host)
            pool.addCommand(cmd)
        pool.join()

        #This will raise ExecutionError exception if even a single command fails
        pool.check_results()

class ListPackages(Operation):
    '''  
        Lists all the packages present in 
        $GPHOME/share/packages/archive
    '''

    def __init__(self):
        pass 

    def execute(self):
        #Get all gppkgs from archive
        package_list = glob.glob(GPPKG_ARCHIVE_PATH+'/*.gppkg')

        package_name_list = []

        for pkg in package_list:
            pkg_name = pkg.split('/')[-1]
            package_name_list.append(pkg_name[:pkg_name.index('-', pkg_name.index('-') + 1)])

        return package_name_list

class TransferPackageContents(Operation):
    '''
      Transfer the package contents which have been upacked in 
      the $GPHOME/.tmp directory to the segment hosts 
      and also the standby
    '''
    
    def __init__(self, master_host, standby_host, segment_host_list):
        self.segment_host_list = segment_host_list
        self.standby_host = standby_host
        self.master_host = master_host

    def execute(self):
          
        pool = WorkerPool()
         
        #clean up the .tmp dir first before transferring
        for host in self.segment_host_list:
            if CheckRemoteDir(TEMP_EXTRACTION_PATH, host).run():
                RemoveRemoteTree(TEMP_EXTRACTION_PATH, host).run()

        #transfer to segments
        for host in self.segment_host_list:
            logger.info('Transferring package to segment %s' % host)
            scp = Scp(name = 'copy gppkg to segments', srcFile = TEMP_EXTRACTION_PATH, dstFile = TEMP_EXTRACTION_PATH, srcHost = None, dstHost = host, recursive = True)
            pool.addCommand(scp)
        pool.join()
         
        pool.check_results()
          
        #clean up the .tmp dir first before transferring
        if self.standby_host:
            if CheckRemoteDir(TEMP_EXTRACTION_PATH, self.standby_host).run():
                RemoveRemoteTree(TEMP_EXTRACTION_PATH, self.standby_host).run()

        #transfer to standby
        if self.standby_host:
            logger.info('Transferring package to standby %s' % self.standby_host)
            scp = Scp(name = 'copy gppkg to standby', srcFile = TEMP_EXTRACTION_PATH, dstFile = TEMP_EXTRACTION_PATH, srcHost =  None, dstHost = self.standby_host, recursive = True)
            scp.run(validateAfter = True)

class CleanupDir(Operation):
    '''
        Cleans up the given dir 
        Returns True if either the dir is already removed
        or if we were able to remove the dir successfully
        False for other errors
    '''

    def __init__(self, dir_path):
        self.dir_path = dir_path

    def execute(self):
       
        dir_path = self.dir_path

        logger.debug('Cleaning up %s' % dir_path)

        #If file does not exist, nothing to remove
        #So we return true
        if not os.path.exists(dir_path):
            return True 

        if os.path.isdir(dir_path):
            shutil.rmtree(dir_path)
        else:
            return False
        
        return True

class IsRpmInstalled(Operation):
    '''
        Returns true if an rpm is installed 
        False otherwise
    '''
    
    def __init__(self, rpm):
        self.rpm = rpm

    def execute(self):

        #Remove the '.rpm' extension
        rpm = self.rpm[:-4]
     
        try:
            cmd_str = 'rpm -q %s --dbpath %s' % (rpm, RPM_DATABASE)
            LocalCommand(cmd_str).run()

            return True

        except ExecutionError:
            return False

class IsVersionCompatible(Operation):
    '''
        Returns True if the gppkg is compatible 
        with the gpdb version that has been installed
    '''
    def __init__(self, gppkg):
        self.gppkg = gppkg

    def execute(self):

        gppkg = self.gppkg

        gpdb_version = self._get_gpdb_version()

        logger.debug('GPDB Version = %s' % gpdb_version)
        logger.debug('GPDB min = %s' % gppkg.gpdbmin)
        logger.debug('GPDB max = %s' % gppkg.gpdbmax)

        if gpdb_version is None:
            logger.error('Could not determine gpdb version')
            return False
 
        if gppkg.gpdbmin == gppkg.gpdbmax:
            if gpdb_version != gppkg.gpdbmin:
                logger.error('%s requires gpdb version %s' % (gppkg.pkg, gppkg.gpdbmin))
                return False

        if gpdb_version < gppkg.gpdbmin:
            logger.error('%s requires gpdb version >= %s' % (gppkg.pkg, gppkg.gpdbmin))
            return False 
        elif gpdb_version > gppkg.gpdbmax:
            logger.error('%s requires gpdb version <= %s' % (gppkg.pkg, gppkg.gpdbmax))
            return False 

        return True

    def _get_gpdb_version(self):
        '''
            Get the version of the current GPDB
            Returns a string consisting of the major 
            release version
        '''

        logger.debug('_get_gpdb_version')
        self.gphome = gp.get_gphome()
        version = gp.GpVersion.local('local GP software version check', self.gphome)
        gpdb_version = GpVersion(version.strip()) 
        return gpdb_version

class StorePackageInArchive(Operation):
    '''
        Stores gppkg in $GPHOME/arhive
        The packages need to be store in both 
        Master and the Standby
    '''

    def __init__(self, gppkg, standby):
        self.gppkg = gppkg
        self.standby = standby

    def execute(self):

        gppkg = self.gppkg
      
        #Store the package on the standby master
        if self.standby:
            if not CheckRemoteDir(GPPKG_ARCHIVE_PATH, self.standby).run():
                MakeRemoteDir(GPPKG_ARCHIVE_PATH, self.standby).run()

            #Scp the package 
            scp = Scp(name = 'copy gppkg to segments', srcFile = gppkg.abspath, dstFile = os.path.join(GPPKG_ARCHIVE_PATH, gppkg.pkg), srcHost = None, dstHost = self.standby, recursive = False)
            scp.run(validateAfter = True)

        #Store the package from the archive on master
        if not os.path.exists(GPPKG_ARCHIVE_PATH):
            os.mkdir(GPPKG_ARCHIVE_PATH)
        shutil.copyfile(gppkg.abspath, os.path.join(GPPKG_ARCHIVE_PATH, gppkg.pkg))

class RemovePackageFromArchive(Operation):
    '''
        Removes packages from $GPHOME/archive
        The packages need to be removed from both
        Master and Standby
    '''

    def __init__(self, pkg_path, standby):
        self.pkg_path = pkg_path
        self.standby = standby

    def execute(self):
     
        #gppkg = self.gppkg

        #Remove from the master
        if os.path.exists(os.path.join(GPPKG_ARCHIVE_PATH, self.pkg_path)):
            os.remove(os.path.join(GPPKG_ARCHIVE_PATH, self.pkg_path))

        #Remove from the standby
        if self.standby:
            if not CheckRemoteDir(GPPKG_ARCHIVE_PATH, self.standby).run():
                return
            else:
                RemoveRemoteFile(os.path.join(GPPKG_ARCHIVE_PATH, self.pkg_path), self.standby).run()

##############################################################################
#    This class can be replaced completely by using a single command         #
#    rpm -i <list of all rpms>                                               #
#    If some of the rpms in the list are already installed,                  #
#    the rpm gives an error saying that these rpms are already               #
#    installed. We can parse this stderr message and remove from             #
#    the list those rpms which are already installed.                        #
##############################################################################
class GetRpmInstallString(Operation):
    '''
        Return the string containing the list of RPMS to be installed
    '''

    def __init__(self, gppkg):
        self.gppkg = gppkg

    def execute(self):
        
        gppkg = self.gppkg

        queue = Queue()
        rpm_list = [] 
        used = dict() 
        
        queue.put(gppkg.main_rpm)

        while not queue.empty():
            rpm = queue.get()  
            rpm_list.append(rpm)
            used[rpm] = True            #TODO: Check this in before submitting code

            #check dependencies
            cmd_str = 'rpm -qpR %s' % (os.path.join(TEMP_EXTRACTION_PATH, rpm))
            results = LocalCommand(cmd_str).run()

            result = results.stdout
            result = result.split('\n') 

            #If this dependency is not in the gppkg,
            #notify the user of error and exit
            for dep in result:
                if dep is not None:
                    dep = dep.strip()

                if dep == '(none)' or len(dep) == 0 or 'rpmlib(CompressedFileNames)' in dep or 'rpmlib(PayloadFilesHavePrefix)' in dep:
                    break 

                if dep in gppkg.dependencies:
                    if not IsRpmInstalled(dep).run() and dep not in used.keys():
                        queue.put(dep)
                else:
                    raise MissingDependencyError(dep)

        rpm_install_string = ' '.join(rpm_list)

        return rpm_install_string

class ExtractPackage(Operation):
    '''
        Extract the contents of the package into the temp folder
    '''

    def __init__(self, gppkg):
        self.gppkg = gppkg 

    def execute(self):
      
        try:
            gppkg = self.gppkg

            #clean up tmp extraction folder
            if os.path.exists(TEMP_EXTRACTION_PATH) and not CleanupDir(TEMP_EXTRACTION_PATH).run():
                logger.error('Could not clean temp folder')
                raise IOError

            #untar the package into tmp folder
            tarinfo = tarfile.open(gppkg.abspath)
            tarinfo.extractall(TEMP_EXTRACTION_PATH)
 
            #move all the deps into same folder as the main rpm
            path = os.path.join(TEMP_EXTRACTION_PATH, DEPS_DIR)
            if os.path.exists(path):
                for cur_file in os.listdir(path):
                    shutil.move(os.path.join(TEMP_EXTRACTION_PATH, DEPS_DIR, cur_file), TEMP_EXTRACTION_PATH)

        finally:
            if tarinfo:
                tarinfo.close()

#TODO: Try to make this operation idempotent
class InstallGppkg(Operation):
    '''
        Main Class for installing a gppkg
    '''
    def __init__(self, gppkg, master_host, standby_host, segment_host_list):
        self.gppkg = gppkg
        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list

    def execute(self):
       
        rpm_installation_cmd = 'rpm -i %s --dbpath %s --prefix=%s'   

        gppkg = self.gppkg

        logger.info('Installing package %s' % gppkg.pkg)

        try:

            #Check the GPDB requirements
            if not IsVersionCompatible(gppkg).run():
                raise GpdbVersionError

            #TODO: Temporarily disabled. Will enable this for the final version
            #OS compatibility
            if gppkg.os.lower() != platform.system().lower():
                raise OSCompatibilityError(gppkg.os, platform.system().lower())

            #TODO: Temporariliy disabled. will enable this for the final version
            #architecture compatibility
            if gppkg.architecture.lower() !=  platform.machine().lower():
                raise ArchCompatibilityError(gppkg.architecture, platform.machine().lower())
 
            #check if already installed
            if IsRpmInstalled(gppkg.main_rpm).run():
                raise AlreadyInstalledError(gppkg.pkg) 

            #Extract the package contents
            ExtractPackage(gppkg).run()

            #Transfer the gppkgs to the new segments 
            #before actual installation
            TransferPackageContents(self.master_host, self.standby_host, self.segment_host_list).run()
     
            #perform any preinstallation steps 
            self._perform_pre_install(gppkg)
     
            #get the rpms that need to be installed
            install_string = GetRpmInstallString(gppkg).run()

            #prepare the command string
            cmd_str = rpm_installation_cmd % (install_string, RPM_DATABASE, RPM_INSTALLATION_PATH)
   
            #chdir to the correct directory where files are located
            old_dir = os.getcwd()
            os.chdir(TEMP_EXTRACTION_PATH)
 
            #First install on the segments
            RemoteCommand(cmd_str, self.segment_host_list).run()
 
            #Second install on the standby
            if self.standby_host:
                RemoteCommand(cmd_str, [self.standby_host]).run()
     
            #Third install on the master
            LocalCommand(cmd_str).run()
 
            #change back to old directory
            os.chdir(old_dir)
 
            #run postinstall steps
            self._perform_post_install(gppkg)
 
            #Store the gppkg in $GPHOME/addons/archive
            StorePackageInArchive(gppkg, self.standby_host).run()

            logger.info('%s successfully installed.' %(gppkg.pkg))

        finally:
            if  os.path.exists(TEMP_EXTRACTION_PATH) and not CleanupDir(TEMP_EXTRACTION_PATH).run():
                raise IOError

    def _perform_pre_install(self, gppkg):
        '''
            Perform any pre installation steps 
            that have been specified in the yml file 
        '''

        logger.debug('_perform_pre_install')

        if gppkg.preinstall is None:
            logger.info('No preinstall hooks')
            return 
        try:

            old_dir = os.getcwd()
            os.chdir(TEMP_EXTRACTION_PATH)

            for preinst in gppkg.preinstall:
                key = preinst.keys()
                if key is None:
                    return
                key_str = key[0]
                if key_str.lower() == 'master': 
                    if self.standby_host:
                        RemoteCommand(preinst[key_str], [self.standby_host]).run()
                    LocalCommand(preinst[key_str], True).run()
                elif key_str.lower() == 'segment':
                    RemoteCommand(preinst[key_str], self.segment_host_list).run()
           
        finally:
            #Change back to old directory
            os.chdir(old_dir)

    def _perform_post_install(self, gppkg):
        '''
            Perform post installation steps
            that have been specified in the yml file
        '''

        logger.debug('_perform_post_install')

        try:
            
            if gppkg.postinstall is None:
                logger.info('No postinstall hooks')
                return 

            #Change directory to temp directory
            old_dir = os.getcwd()
            os.chdir(TEMP_EXTRACTION_PATH)

            for postinst in gppkg.postinstall:
                key = postinst.keys()
                if key is None:
                    return
                key_str = key[0]
                if key_str.lower() == 'master':
                    if self.standby_host:
                        RemoteCommand(postinst[key_str], [self.standby_host]).run()
                    LocalCommand(postinst[key_str], True).run()
                elif key_str.lower() == 'segment':
                    RemoteCommand(postinst[key_str], self.segment_host_list).run()
            
        finally:
            #Change back to original directory
            os.chdir(old_dir)

#TODO: Try to make this operation idempotent
class UnInstallGppkg(Operation):
    '''
        Uninstall a given gppkg
    '''

    def __init__(self, gppkg, master_host, standby_host, segment_host_list, cleanup_gppkg = False):
        self.gppkg = gppkg
        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list
        self.cleanup_gppkg = cleanup_gppkg

    def execute(self):

        rpm_remove_cmd = 'rpm -e %s --dbpath %s'

        tarinfo = None
        try:
            gppkg = self.gppkg

            #Test to see if the main rpm has been installed or not
            #Since the DB on the master will be identical to the 
            #ones on the segment, it is sufficient to check only that
            #RPM database
            if not self.cleanup_gppkg:
                if not IsRpmInstalled(gppkg.main_rpm).run():
                    raise NotInstalledError(gppkg.pkg)

            #unpack the package so that we can read the uninstall hooks if any
            #clean up tmp extraction folder
            if os.path.exists(TEMP_EXTRACTION_PATH) and not CleanupDir(TEMP_EXTRACTION_PATH).run():
                logger.error('Could not clean temp folder')
                raise IOError

            #untar the package into tmp folder
            tarinfo = tarfile.open(gppkg.abspath)
            tarinfo.extractall(TEMP_EXTRACTION_PATH)
 
            #Transfer the package contents
            TransferPackageContents(self.master_host, self.standby_host, self.segment_host_list).run()

            #Pre uninstall hooks
            self._perform_pre_uninstall(gppkg)

            #perfrom the uninstall. 
            #'-4' to remove the '.rpm' extension
            rpm_uninstall_string = gppkg.main_rpm[:-4] + ' ' 
            for dep in gppkg.dependencies:
                rpm_uninstall_string += (dep[:-4] + ' ')

            #We first do a test removal to see if removing the packages cause any conflicts
            #We filter out all those rpms which cause conflicts and remove the remaining ones
            cmd_str = 'rpm -e --test %s --dbpath %s' % (rpm_uninstall_string, RPM_DATABASE)

            result = None

            #We parse the stderr to make sure that we exit for the right reason
            #E.g of the error on which we want to exit :-
            #error: Failed dependencies:
            #    geosdevel-1.12-1.noarch.rpm is needed by (installed) geos-1.12-1.noarch
            #    geosdevel-1.12-1.noarch.rpm is needed by (installed) postgis-1.13-1.noarch

            try:
                result =  LocalCommand(cmd_str).run() 
            except ExecutionError, ex:
                results =  ex.cmd.get_results().stderr.strip().split('\n')
                for result in results:
                    if 'failed dependencies' in result.lower():     #conflicting dependencies so ignore these and remove the rest
                        rpm_uninstall_string = self._get_filtered_rpm_string(results)            
                        break

            cmd_str = rpm_remove_cmd % (rpm_uninstall_string, RPM_DATABASE)

            try:
                #First uninstall on the segment hosts
                RemoteCommand(cmd_str, self.segment_host_list).run()
            except ExecutionError, ex:
                if self.cleanup_gppkg:
                    results =  ex.cmd.get_results().stderr.strip().split('\n')
                    for result in results:
                        if not 'cannot create' in result.lower():                       #This is for the repackaging feature of rpm. TODO: Probably there is a way to turn this feature off
                            raise ExceptionNoStackTraceNeeded('Error %s' % str(ex))

            try:
                #Next uninstall on the standby host
                if self.standby_host:
                    RemoteCommand(cmd_str, [self.standby_host]).run()
            except ExecutionError, ex:
                if self.cleanup_gppkg:
                    results =  ex.cmd.get_results().stderr.strip().split('\n')
                    for result in results:
                        if not 'cannot create' in result.lower():
                            raise ExceptionNoStackTraceNeeded('Error %s' % str(ex))

            try:
                #Last Uninstall on the master host
                LocalCommand(cmd_str).run()
            except ExecutionError, ex:
                if not self.cleanup_gppkg:
                    results =  ex.cmd.get_results().stderr.strip().split('\n')
                    for result in results:
                        if not 'cannot create' in result.lower():
                            raise ExceptionNoStackTraceNeeded('Error %s' % str(ex))

            #Post uninstall hooks
            self._perform_post_uninstall(gppkg)

            #Remove package from $GPHOME/addons/archive
            RemovePackageFromArchive(gppkg.pkg, self.standby_host).run()
            
            logger.info('%s has been uninstalled.' % (gppkg.pkg))

        finally:
            if tarinfo:
                tarinfo.close()
        
    def _get_filtered_rpm_string(self, results):
        '''
            Filters the results and returns those rpms
            which can be uninstalled safely without
            causing any dependency issues
        '''

        filtered_rpm_string = ' ' 
        filter_set = set() 
        for result in results:
            tokens = result.split(' ')

            for token in tokens:
                if '.rpm' in token:
                    filter_set.add(token.strip())

        if self.gppkg.main_rpm not in filter_set:
            filtered_rpm_string = self.gppkg.main_rpm[:-4] + ' '

        for rpm in self.gppkg.dependencies:
            if rpm not in filter_set:
                filtered_rpm_string = filtered_rpm_string + rpm[:-4] + ' '

        return filtered_rpm_string

    def _perform_pre_uninstall(self, gppkg):
        '''
            Perform pre uninstall steps
            that have been specified in yml file
        '''

        logger.debug('_perform_pre_uninstall')

        if gppkg.preuninstall is None:
            logger.info('No preuninstall hooks')
            return 

        #Change directory to temp directory
        old_dir = os.getcwd()
        os.chdir(TEMP_EXTRACTION_PATH)

        try:
            
            for preuninst in gppkg.preuninstall:
                key = preuninst.keys()
                if key is None:
                    return
                key_str = key[0]
                if key_str.lower() == 'master':
                    if self.standby_host:
                        RemoteCommand(preuninst[key_str], [self.standby_host]).run()
                    LocalCommand(preuninst[key_str], True).run()
                elif key_str.lower() == 'segment':
                    RemoteCommand(preuninst[key_str], self.segment_host_list).run()
            
        finally:
            #Change back to original directory
            os.chdir(old_dir)
            
    def _perform_post_uninstall(self, gppkg):
        '''
            Perform post uninstall steps
            that have been specified in yml file
        '''

        logger.debug('_perform_post_uninstall')

        if gppkg.postuninstall is None:
            logger.info('No postuninstall hooks')
            return 

        #Change directory to temp directory
        old_dir = os.getcwd()

        try:

            for postuninst in gppkg.postuninstall:
                key = postuninst.keys()
                if key is None:
                    return
                key_str = key[0]
                if key_str.lower() == 'master':
                    if self.standby_host:
                        RemoteCommand(postuninst[key_str], [self.standby_host]).run()
                    LocalCommand(postuninst[key_str], True).run()
                elif key_str.lower() == 'segment':
                    RemoteCommand(postuninst[key_str], self.segment_host_list).run()
            
        finally:
            #Change back to original directory
            os.chdir(old_dir)

class QueryGppkg(Operation):
    '''
       Query the gppkg and print out information regarding it
    '''

    def __init__(self, gppkg, query):
        
        self.gppkg = gppkg
        self.query = query

    def execute(self):

        gppkg = self.gppkg

        #print out the information about the package
        if self.query in ['I', '-I', 'info', '--info']:
            print 'Name: %s' % gppkg.pkgname
            print 'Version:    %s' % gppkg.version
            print 'Architecture:    %s' % gppkg.architecture
            print 'OS:         %s' % gppkg.os
            print 'GPDBMin:    %s' % gppkg.gpdbmin
            print 'GPDBMax:    %s' % gppkg.gpdbmax
            print 'Description:    %s' % gppkg.description
            
            return 

        #print out all the dependencies of the package
        elif self.query in ['R', '-R', 'requires', '--requires']:
            for deps in gppkg.dependencies:
                print deps
            return
    
        #print out all the files present in the package
        elif self.query in ['l', '-l', 'list', '--list']:
            for cur_file in gppkg.file_list:
                print cur_file
            return
      
        elif self.query in ['all', '--all']:
            package_name_list = ListPackages().run()
            for pkg_name in package_name_list:
                print pkg_name

        #No query option was entered. So just query the RPM database
        elif IsRpmInstalled(gppkg.main_rpm).run():
            print '%s is installed.' % gppkg.pkgname
        else:
            print '%s is not installed.' % gppkg.pkgname

class BuildGppkg(Operation):
    '''
        Builds a gppkg given a directory containing 
        the spec file, rpms and any pre/post installation scripts
    '''

    def __init__(self, directory):
        self.directory = directory

    def execute(self):
        
        directory = self.directory

        logger.info('Building gppkg')

        #Check if the directory is valid
        if not os.path.exists(directory) or not os.path.isdir(directory):
            logger.error('%s is an Invalid directory' % directory)
            raise BuildPkgError

        filelist = os.listdir(directory)

        #Check for the spec file
        specfile = directory + '/' + SPECFILE_NAME

        if not os.path.exists(specfile):
            logger.error(' Spec file does not exist')
            raise BuildPkgError 

        #parse the spec file and get the name, version and arch 
        #this is used to name the gppkg
        pkg_path_details = self._get_package_name_details(specfile)

        if pkg_path_details is None:
            raise BuildPkgError 

        #The file already exists. Rewrite the original with the new one 
        pkg = pkg_path_details['pkgname'] + '-' + str(pkg_path_details['version']) + '-' + pkg_path_details['os'] + '-' + pkg_path_details['architecture'] + '.gppkg'
        if os.path.exists(pkg):
            os.remove(pkg)

        #Verify the spec file 
        if not self._verify_specfile(specfile, directory):
            raise BuildPkgError 

        #store the directory so that we can 
        #get back after taring the file
        old_dir = os.getcwd()

        #cd to the directory 
        os.chdir(directory)

        #tar and gzip the directory
        #rename the file with .gppkg extension

        try:
            tar = tarfile.open(pkg, 'w:gz')

            for cur_file in filelist:
                tar.add(cur_file)
        finally:
            tar.close()

        #change back to old directory
        os.chdir(old_dir)
    
        #move the gppkg to the new directory
        shutil.move(directory + '/' + pkg, '.')

        logger.info('Completed building gppkg')

    def _get_package_name_details(self, specfile):
        '''
            Get details about the name, version, operating system, architecture 
            of the package. The final gppkg which will be created
            will be named as <name>-<version>-<os>-<arch>.gppkg
        '''

        logger.debug('_get_package_name_details')
        cur_file = None

        with open(specfile) as cur_file:
            yamlfile = yaml.load(cur_file)
    
            tags = yamlfile.keys()

            pkg_path_details = {}

            #return all the required tags as a dict
            for tag in tags:
                if tag.lower() in SPECFILE_REQUIRED_TAGS:
                    pkg_path_details[tag.lower()] = yamlfile[tag]

            return pkg_path_details

    def _verify_specfile(self, specfile, directory):
        '''
            Reads the spec file and makes sure that the tags are correct.
        '''

        logger.debug('_verify_specfile')
        cur_file = None

        try:
            with open(specfile) as cur_file:
                yamlfile = yaml.load(cur_file)

                if not self._verify_tags(yamlfile):
                    return False 

                return True
        except ScannerError, ex:
            return False

    def _verify_tags(self, yamlfile):
        '''
            Verify that the tags are valid.
            Returns true if all tags are valid
            False otherwise
        '''

        logger.debug('_verify_tags')
        tags = yamlfile.keys()
    
        tags = [tag.lower() for tag in tags]

        #check required tags 
        for required_tag in SPECFILE_REQUIRED_TAGS:
            if required_tag not in tags:
                logger.error(' Required tag %s missing in Spec file' % required_tag)
                return False

        #check for invalid tags
        for tag in tags:
            if tag not in SPECFILE_OPTIONAL_TAGS and tag not in SPECFILE_REQUIRED_TAGS:
                logger.error(' Invalid tag %s in Spec file' % tag)
                return False

        return True
        
class UpdateGppkg(Operation):
    '''
        Update to the given gppkg
    '''

    def __init__(self, gppkg, master_host, standby_host, segment_host_list):
        self.gppkg = gppkg
        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list

    def execute(self):

        rpm_update_cmd = 'rpm -U %s --dbpath %s --prefix=%s'

        gppkg = self.gppkg

        #TODO: Check compatibility
        if not IsVersionCompatible(gppkg).run():
            raise GpdbVersionError

        #Extract the contents of the package into a temp folder
        ExtractPackage(gppkg).run()

        #Transfer package
        TransferPackageContents(self.master_host, self.standby_host, self.segment_host_list).run() 

        #get the installation string
        rpm_install_string = GetRpmInstallString(gppkg).run()

        #perform update using rpm -U 
        #First we test if update is possible. If it fails due to dependencies we abort the update.
        cmd_str = 'rpm -U %s --test --dbpath %s --prefix=%s' % (rpm_install_string, RPM_DATABASE, RPM_INSTALLATION_PATH) 

        try:
            LocalCommand(cmd_str).run()
        except ExecutionError, ex:
            results = ex.cmd.get_results().stderr.split('\n')
            for result in results:
                if 'failed dependency'in result.lower():
                    raise RequiredDependencyError
        
        cmd_str = rpm_update_cmd % (rpm_install_string, RPM_DATABASE, RPM_INSTALLATION_PATH)

        #change the directory
        old_dir = os.getcwd()
        os.chdir(TEMP_EXTRACTION_PATH)

        #first the segments
        RemoteCommand(cmd_str, self.segment_host_list).run()

        #next standby
        if self.standby_host:
            RemoteCommand(cmd_str, [self.standby_host]).run()

        #last master
        LocalCommand(cmd_str).run()

        #change back to original directory
        os.chdir(old_dir)

        #Remove older versions of the package from archive
        file_list = set()
        if os.path.exists(GPPKG_ARCHIVE_PATH):
            for filename in os.listdir(GPPKG_ARCHIVE_PATH):
                if filename[:filename.index('-')] == gppkg.pkgname:
                    file_list.add(os.path.join(GPPKG_ARCHIVE_PATH, filename)) 

        #Remove on the standby
        for filename in file_list:
            RemovePackageFromArchive(filename, self.standby_host).run()

        #Store the latest version of the file in the archive
        StorePackageInArchive(gppkg, self.standby_host).run()

class CleanGppkg(Operation):
    '''
        Cleans up the Gppkg from the cluster in case of partial
        installation or removal. This might not be required if 
        we can make the install and uninstall options idempotent.
        This operation is exactly the same as remove but we dont 
        check on each host to see if the rpm is installed or not.
    '''

    def __init__(self, gppkg, master_host, standby_host, segment_host_list):
        self.gppkg = gppkg 
        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list

    def execute(self):
        UnInstallGppkg(self.gppkg, self.master_host, self.standby_host, self.segment_host_list, cleanup_gppkg = True).run()
