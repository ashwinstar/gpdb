#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST

import tarfile
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
    from gppylib.operations.unix import RemoveRemoteTree, CheckRemoteDir
except ImportError, e:
    sys.exit('Operation: Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(e)) 

logger = gplog.get_default_logger()

GPHOME = gp.get_gphome() 

SPECFILE_NAME = 'gppkg_spec.yml'
SPECFILE_REQUIRED_TAGS = ['pkgname', 'version', 'architecture', 'os', 'description', 'gpdbmin', 'gpdbmax']
SPECFILE_OPTIONAL_TAGS = ['preinstall', 'postinstall', 'preuninstall', 'postuninstall']

RPM_DATABASE = os.path.join(GPHOME, 'addons/.gprpmdb')
RPM_INSTALLATION_PATH = GPHOME
RPM_INSTALLATION_CMD = 'rpm -i %s --dbpath %s --prefix=%s'   
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

    def __init__(self, cmd_str):
        self.cmd_str = cmd_str

    def execute(self):

        cmd = Command(name = 'LocalCommand', cmdStr = self.cmd_str)
        cmd.run(validateAfter = True)
        return cmd.get_results()

class RemoteCommand(Operation):
    '''
        Executes a command on a remote host
    '''

    def __init__(self, cmd_str, host_list):
        self.cmd_str = cmd_str
        self.host_list = host_list
       
    def execute(self):
        
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
            if host != self.master_host:        #Segments might exist on the master itself. So we need to check for this.
                if CheckRemoteDir(TEMP_EXTRACTION_PATH, host).run():
                    RemoveRemoteTree(TEMP_EXTRACTION_PATH, host).run()

        # self._remote_command(cmd_str, host_list)
         
        #transfer to segments
        for host in self.segment_host_list:
            if host != self.master_host:
                logger.info('Transferring package to segment %s' % host)
                scp = Scp(name = 'copy gppkg to segments', srcFile = TEMP_EXTRACTION_PATH, dstFile = TEMP_EXTRACTION_PATH, srcHost = None, dstHost = host, recursive = True)
                pool.addCommand(scp)
        pool.join()
         
        pool.check_results()
          
        #clean up the .tmp dir first before transferring
        if self.standby_host:
            if CheckRemoteDir(TEMP_EXTRACTION_PATH, self.standby_host).run():
                RemoveRemoteTree(TEMP_EXTRACTION_PATH, host).run()

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

        logger.info('Cleaning up %s' % dir_path)

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
            results = LocalCommand(cmd_str).run()

            return True

        except ExecutionError:
            return False

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

        q = Queue()
        rpm_list = [] 
        used = dict() 
        
        q.put(gppkg.main_rpm)

        while not q.empty():
            rpm = q.get()  
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

                if dep == '(none)' or len(dep) == 0:
                    break 

                if dep in gppkg.dependencies:
                    if not IsRpmInstalled(dep).run() and dep not in used.keys():
                        q.put(dep)
                else:
                    raise MissingDependencyError(dep)

        rpm_install_string = ' '.join(rpm_list)

        return rpm_install_string

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
       
        gppkg = self.gppkg

        logger.info('Installing package %s' % gppkg.pkg_path)

        try:

            #Check the GPDB requirements
            gpdb_version = self._get_gpdb_version()

            if gpdb_version is None:
                logger.error('Could not determine gpdb version')
                raise GpdbVersionError
 
            if gppkg.gpdbmin == gppkg.gpdbmax:
                if gpdb_version != str(gppkg.gpdbmin):
                    logger.error('%s requires gpdb version %s' % (gppkg.pkg_path, gppkg.gpdbmin))
                    raise GpdbVersionError

            if gpdb_version < str(gppkg.gpdbmin):
                logger.error('%s requires gpdb version >= %s' % (gppkg.pkg_path, gppkg.gpdbmin))
                raise GpdbVersionError
            elif gpdb_version > str(gppkg.gpdbmax):
                logger.error('%s requires gpdb version <= %s' % (gppkg.pkg_path, gppkg.gpdbmax))
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
                raise AlreadyInstalledError(gppkg.pkg_path) 
    
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

            #Transfer the gppkgs to the new segments 
            #before actual installation
            TransferPackageContents(self.master_host, self.standby_host, self.segment_host_list).run()
     
            #perform any preinstallation steps 
            self._perform_pre_install(gppkg)
     
            #get the rpms that need to be installed
            install_string = GetRpmInstallString(gppkg).run()

            #prepare the command string
            cmd_str = RPM_INSTALLATION_CMD % (install_string, RPM_DATABASE, RPM_INSTALLATION_PATH)
   
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
            if not os.path.exists(GPPKG_ARCHIVE_PATH):
                os.mkdir(GPPKG_ARCHIVE_PATH)
            shutil.copyfile(gppkg.abspath, os.path.join(GPPKG_ARCHIVE_PATH, gppkg.pkg_path))
    
        finally:
            if  os.path.exists(TEMP_EXTRACTION_PATH) and not CleanupDir(TEMP_EXTRACTION_PATH).run():
                raise IOError

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
        gpdb_version = gpdb_version.getVersionRelease()
        self.version = gpdb_version
        return self.version

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
                    result = LocalCommand(preinst[key_str]).run()
                elif key_str.lower() == 'segment':
                    results = RemoteCommand(preinst[key_str], self.segment_host_list).run()
           
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
                    result = LocalCommand(postinst[key_str]).run()
                elif key_str.lower() == 'segment':
                    results = RemoteCommand(postinst[key_str], self.segment_host_list).run()
            
        finally:
            #Change back to original directory
            os.chdir(old_dir)

#TODO: Try to make this operation idempotent
class UnInstallGppkg(Operation):
    '''
        Uninstall a given gppkg
    '''

    def __init__(self, gppkg, master_host, standby_host, segment_host_list):
        self.gppkg = gppkg
        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list

    def execute(self):

        gppkg = self.gppkg

        #Test to see if the main rpm has been installed or not
        #Since the DB on the master will be identical to the 
        #ones on the segment, it is sufficient to check only that
        #RPM database
        if not IsRpmInstalled(gppkg.main_rpm).run():
            raise NotInstalledError(gppkg.pkg_path)

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

        #perfrom the uninstall. This just involves removing the main rpm.
        #We retain the other dependency rpms installed as other packages
        #might be using them
        rpm = gppkg.main_rpm[:-4]       #getting rid of the '.rpm' extension
        #What if the main rpm is a dependency of another rpm. e.g R is a dependency for PL/R
        #TODO: We need to check for that before uninstalling
        cmd_str = 'rpm -e --test %s --dbpath %s' % (rpm, RPM_DATABASE)
        if LocalCommand(cmd_str).run().rc != 0: 
            raise RequiredDependencyError            
        cmd_str = 'rpm -e %s --dbpath %s' % (rpm, RPM_DATABASE)

        #First uninstall on the segment hosts
        RemoteCommand(cmd_str, self.segment_host_list).run()

        #Next uninstall on the standby host
        if self.standby_host:
            RemoteCommand(cmd_str, [self.standby_host]).run()

        #Last Uninstall on the master host
        LocalCommand(cmd_str).run()

        #Post uninstall hooks
        self._perform_post_uninstall(gppkg)

        #Remove package from $GPHOME/addons/archive
        if os.path.exists(os.path.join(GPPKG_ARCHIVE_PATH, gppkg.pkg_path)):
            os.remove(os.path.join(GPPKG_ARCHIVE_PATH, gppkg.pkg_path))
        
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
                    result = LocalCommand(preuninst[key_str]).run()
                elif key_str.lower() == 'segment':
                    results = RemoteCommand(preuninst[key_str], self.segment_host_list).run()
            
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
                    result = LocalCommand(postuninst[key_str]).run()
                elif key_str.lower() == 'segment':
                    results = RemoteCommand(postuninst[key_str], self.segment_host_list).run()
            
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
            print 'Name: %s' % gppkg.pkg_path[:gppkg.pkg_path.index('-')]
            print 'Version:    %s' % gppkg.version
            print 'OS: %s' % gppkg.os
            print 'Architecture: %s' % gppkg.architecture
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

        #The file already exists 
        pkg_path = pkg_path_details['pkgname'] + '-' + str(pkg_path_details['version']) + '-' + pkg_path_details['os'] + '-' + pkg_path_details['architecture'] + '.gppkg'
        if os.path.exists(pkg_path):
            logger.error(' %s already exists !' % pkg_path)
            raise BuildPkgError 

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
        tar = tarfile.open(pkg_path, 'w:gz')

        for cur_file in filelist:
            tar.add(cur_file)

        tar.close()

        #change back to old directory
        os.chdir(old_dir)
    
        #move the gppkg to the new directory
        shutil.move(directory + '/' + pkg_path, '.')

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
            It also verifies that if any installation hooks are specified,
            they exist in the gppkg.
        '''

        logger.debug('_verify_specfile')
        cur_file = None

        try:
            with open(specfile) as cur_file:
                yamlfile = yaml.load(cur_file)

                if not self._verify_tags(yamlfile):
                    return False 

                if not self._verify_scripts(yamlfile, directory):
                    return False

                return True
        except ScannerError, e:
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

    def _verify_scripts(self, yamlfile, directory):

        '''
           Verify that the installation scripts are present in the directory (if specified in the yml file)
           Returns true if the scripts are present (if specified in the yml file)
           False otherwise
        '''

        logger.debug('_verify_scripts')
        installsteps = []            #contains the steps mentioned in pre/post install hooks
        tags = yamlfile.keys()       #contains the tags e.g pkgName, Version, PreInstall etc

        #Make a list of the pre/post install hooks
        for tag in tags:
            if tag.lower() in SPECFILE_OPTIONAL_TAGS:
                installsteps += yamlfile[tag]

        for installstep in installsteps:
            keys = installstep.keys()
            for key in keys:
                step = installstep[key].strip()    
                step_toks = step.split(' ')
                
                for tok in step_toks:
                    if tok.find('.') != -1:                                 # the scripts must have extensions                      
                        if directory and not os.path.exists(directory + '/' + tok):                           
                            logger.error(' File %s does not exist' % tok)
                            return False

        return True
        
def UpgradeGppkg(Operation):
    
    '''
        Upgrade to the given gppkg
    '''
    #Check compatibility
    #Check if not installed
    #Transfer package
    #perform upgrade using rpm -U

