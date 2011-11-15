# Line too long - pylint: disable=C0301
# Copyright (c) Greenplum Inc 2011. All Rights Reserved.

from contextlib import closing
import os
import platform
from Queue import Queue
import shutil
import sys
import tarfile

try:
    from gppylib import gplog
    from gppylib.commands import gp
    from gppylib.commands.base import Command, REMOTE, WorkerPool, ExecutionError
    from gppylib.commands.unix import Scp
    from gppylib.gpversion import GpVersion
    from gppylib.mainUtils import ExceptionNoStackTraceNeeded
    from gppylib.operations import Operation
    from gppylib.operations.utils import RemoteOperation, ParallelOperation
    from gppylib.operations.unix import CheckDir, MakeDir, RemoveRemoteTree, RemoveRemoteFile, CheckRemoteDir, MakeRemoteDir, CheckRemoteFile, ListRemoteFilesByPattern, ListFilesByPattern

    import yaml   
    from yaml.scanner import ScannerError
except ImportError, ex:
    sys.exit('Operation: Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(ex)) 

logger = gplog.get_default_logger()

GPHOME = gp.get_gphome() 

GPPKG_EXTENSION = '.gppkg'
SPECFILE_NAME = 'gppkg_spec.yml'
SPECFILE_REQUIRED_TAGS = ['pkgname', 'version', 'architecture', 'os', 'description', 'gpdbversion']
SPECFILE_OPTIONAL_TAGS = ['preinstall', 'postinstall', 'preuninstall', 'postuninstall']

RPM_DATABASE_PATH = 'share/packages/database'
RPM_DATABASE = os.path.join(GPHOME, RPM_DATABASE_PATH)
RPM_INSTALLATION_PATH = GPHOME

ARCHIVE_PATH = 'share/packages/archive'
GPPKG_ARCHIVE_PATH = os.path.join(GPHOME, ARCHIVE_PATH)

# TODO: AK: Shouldn't this be "$GPHOME/.tmp"? 
# i.e. what if remote host has its $GPHOME elsewhere?
TEMP_EXTRACTION_PATH = GPHOME + '/.tmp'
DEPS_DIR = 'deps'

class GpdbVersionError(Exception):
    
    '''
        Exception to notify that the gpdb version
        does not match
    '''
    pass

class AlreadyInstalledError(Exception):
    def __init__(self, package_name):
        Exception.__init__(self, '%s is already installed.' % package_name)

class NotInstalledError(Exception):
    def __init__(self, package_name):
        Exception.__init__(self, '%s is not installed.' % package_name)

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

class Gppkg:

    '''
        This class stores all the information about a gppkg
    '''
                
    def __init__(self, pkg, pkgname, main_rpm, version, architecture, os, gpdbversion, description, abspath, preinstall, postinstall, preuninstall, postuninstall, dependencies, file_list):
        ''' 
            The constructor takes the following arguments
            pkg             The complete package name e.g pgcrypto-1.0-Darwin-i386.gppkg
            pkgname         The name of the package as specified in the spec file
            main_rpm        The name of the main rpm. e.g PL/R, PostGIS etc
            version         The version of the gppkg
            architecture    The architecture for which the package is built
            os              The operating system for which the package is built
            gpdbversion     The Greenplum Database version for which package is built
            description     A short description for the package
            abspath         This is the absolute path where the package sits on the host
            preinstall      The cluster level preinstallation hooks
            postinstall     The cluster level postinstallation hooks
            preuninstall    The cluster level preuninstallation hooks
            postuninstall   The cluster level postuninstallation hooks
            dependencies    The dependencies of the package. e.g Geos, Proj in case of PostGIS
            file_list       The list of files present in the package
        '''        

        logger.debug('Gppkg Constructor')

        self.pkg = pkg          
        self.pkgname = pkgname  
        self.main_rpm = main_rpm
        self.version = version 
        self.architecture = architecture
        self.os = os
        self.gpdbversion = gpdbversion
        self.description = description 
        self.abspath = abspath 
        self.preinstall = preinstall
        self.postinstall = postinstall
        self.preuninstall = preuninstall
        self.postuninstall = postuninstall
        self.dependencies = dependencies
        self.file_list = file_list

    @staticmethod
    def from_package_path(pkg_path):
        '''
             This method takes a package as the argument and 
             obtains all the information about the package
             Details include name, arch, OS, version, description, dependencies,
             list of files present in the package and returns a gppkg object
        '''
        
        logger.debug('from_package_path')
           
        if not os.path.exists(pkg_path):
            logger.error('Cannot find package %s' % pkg_path)
            raise IOError 

        #We check for a directory first because 
        #is_tarfile does not accept directories as path names
        if os.path.isdir(pkg_path):
            logger.error('%s is a directory !' % pkg_path)
            raise IOError 

        if not tarfile.is_tarfile(pkg_path) or not pkg_path.endswith(GPPKG_EXTENSION):
            logger.error('%s is Not a valid package' % pkg_path)
            raise IOError 

        if os.path.getsize(pkg_path) == 0:
            logger.error('Package is empty')
            raise IOError 

        pkg = {}

        # XXX: AK: It's purely coincidence that the optional tags are lists.
        for tag in SPECFILE_REQUIRED_TAGS:
            pkg[tag] = ''
        for tag in SPECFILE_OPTIONAL_TAGS:
            pkg[tag] = []

        pkg['file_list'] = []
        pkg['dependencies'] = []
    
        with closing(tarfile.open(pkg_path, 'r:gz')) as tarinfo:
            #store the list of all files present in the archive
            archive_list = tarinfo.getnames()
            pkg["file_list"] = archive_list 

            #The spec file has to be called gppkg_spec
            #so there will only be one such file,
            #so we dont need to worry about the loop
            #overwriting the 'specfile' variable with different values
            for cur_file in archive_list:
                if cur_file.endswith(SPECFILE_NAME):
                    specfile = tarinfo.extractfile(cur_file) 

            yamlfile = yaml.load(specfile)
            keys = yamlfile.keys()

        #store all the tags
        for key in keys:
            pkg[key.lower()] = yamlfile[key]    
        
        #update the pkgpath
        pkg['pkg'] = os.path.split(pkg_path)[-1] 

        #make the version as string
        pkg['version'] = str(pkg['version'])

        #Convert the required version to a GpVersion
        pkg['gpdbversion'] = GpVersion(str(pkg['gpdbversion']))

        #update the absolute path
        pkg['abspath'] = pkg_path

        #store all the dependencies of the gppkg
        for cur_file in archive_list:
            if cur_file.find('deps/') != -1 and cur_file.endswith('.rpm'):
                pkg['dependencies'].append(cur_file[cur_file.rfind('/') + 1:])    

        #store the main rpm 
        for cur_file in archive_list:
            if cur_file.find('deps/') == -1 and cur_file.endswith('.rpm'):
                pkg['main_rpm'] = cur_file

        gppkg = Gppkg(**pkg)

        return gppkg

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
    """
    Executes commands on remote hosts

    TODO: AK: Isn't this the exact same thing as gpssh?! 
    """

    def __init__(self, cmd_str, host_list):
        self.cmd_str = cmd_str
        self.host_list = host_list
       
    def execute(self):
       
        logger.debug(self.cmd_str)

        # Create Worker pool
        # and add commands to it
        pool = WorkerPool()

        for host in self.host_list:
            cmd = Command(name = 'Remote Command', cmdStr = self.cmd_str, ctxt = REMOTE, remoteHost = host)
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
        # Ensure archive path exists
        # TODO: AK: In hindsight, this should've been named MakeDirP,
        # to reflect that it won't blow up if the path already exists.
        MakeDir(GPPKG_ARCHIVE_PATH).run()

        package_list = ListFilesByPattern(GPPKG_ARCHIVE_PATH, '*' + GPPKG_EXTENSION).run() 

        package_name_list = []

        for pkg in package_list:
            pkg_name = pkg.split('/')[-1]
            package_name_list.append(pkg_name[:pkg_name.index('-', pkg_name.index('-') + 1)])

        return package_name_list

class ArchivePackageOnHost(Operation):
    '''
        Stores a package in $GPHOME/share/packages/archive
        on a specified host.
    '''

    def __init__(self, host, gppkg):
        self.host = host
        self.gppkg = gppkg

    def execute(self):

        if not CheckRemoteDir(GPPKG_ARCHIVE_PATH, self.host).run():
            MakeRemoteDir(GPPKG_ARCHIVE_PATH, self.host).run()    
        
        scp = Scp(name = 'copy gppkg to segments', srcFile = os.path.join(GPPKG_ARCHIVE_PATH, self.gppkg), dstFile = os.path.join(GPPKG_ARCHIVE_PATH, self.gppkg), 
                srcHost = None, dstHost = self.host, recursive = True)
        scp.run(validateAfter = True)

class UnArchivePackageOnHost(Operation):
    '''
        Removes a package from $GPHOME/share/packages/archive
        on a specified host
    '''

    def __init__(self, host, gppkg):
        self.host = host
        self.gppkg = gppkg

    def execute(self):

        pkg = os.path.join(GPPKG_ARCHIVE_PATH, self.gppkg)
        if not CheckRemoteFile(pkg, self.host).run():
            return
            
        RemoveRemoteFile(pkg, self.host).run()
            

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
        required_gpdb_version = gppkg.gpdbversion

        logger.debug('Greenplum Database Version = %s' % gpdb_version)
        logger.debug('Required Greenplum Database version = %s' % required_gpdb_version)

        if gpdb_version is None:
            logger.error('Could not determine Greenplum Database version')
            return False
        
        if not required_gpdb_version.isVersionRelease(gpdb_version):
            logger.error('%s requires Greenplum Database version %s' % (gppkg.pkgname, required_gpdb_version))
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



class ValidateInstallRpms(Operation):
    """
    Ensure that the given rpms can be installed safely. This is accomplished mainly
    through use of rpm --test. This command will have one of a few outcomes:
    1) A return code of 0, indicating the installation should proceed smoothly
    2) A non-zero return code, and stderr indicating some of the rpms are already installed.
       We simply omit such rpms from the returned list of rpms, indicating to the caller
       that to be successful, installation should only be attempted on the filtered list of rpms.
    3) A non-zero return code, and stderr indicating that a failed dependency issue will arise
       This scenario must result in a MissingDependencyError. 

    TODO: This is depending on ExtractPackage having put the dependencies in this same directory.
    TODO: Use regexes for more reliable string matching. CR-2865#c20112
    """
    def __init__(self, gppkg):
        self.gppkg = gppkg
    def execute(self):
        rpm_set = set([self.gppkg.main_rpm] + self.gppkg.dependencies)
        rpm_install_string = ' '.join([os.path.join(TEMP_EXTRACTION_PATH, rpm) for rpm in rpm_set])
        rpm_install_command = 'rpm --test -i %s --dbpath %s --prefix %s' % (rpm_install_string, RPM_DATABASE, RPM_INSTALLATION_PATH)
        cmd = Command('Validating rpm installation', rpm_install_command)
        logger.info(cmd)        # TODO: AK: This should be debug(), but RMI cannot propagate a log level.

        try:
            cmd.run(validateAfter = True)
        except ExecutionError, e:
            lines = e.cmd.get_results().stderr.splitlines()

            # Forking between code paths 1 and 2 depends on some meaningful stderr
            # Without such stderr, we must bubble up the ExecutionError.
            if len(lines) == 0:
                raise

            if 'failed dependencies' in lines[0].lower():
                # Code path 3 (see docstring)
                # example stderr:
                # error: Failed dependencies:
                #    geos-3.2-2.x86_64.rpm is needed by postgis-1-0.x86_64

                # TODO: AK: Dependencies should be parsed out here and used to initialize
                # this MissingDependencyError. However, this exception does not support
                # multiple missing dependencies. Some refactoring work is needed in both places.
                logger.error(e.cmd.get_results().stderr)
                raise MissingDependencyError('')

            # Code path 2, possibly (see docstring)
            # example stderr:
            #    package geos-3.2-2.x86_64 is already installed
            #    package proj-4.7-0.x86_64 is already installed
            #    package postgis-1-0.x86_64 is already installed
            for line in lines:
                if 'already installed' in line.lower():
                    package_name = line.split()[1]
                    rpm_name = "%s.rpm" % package_name
                    rpm_set.remove(rpm_name)
                else:
                    # This is unexpected, so bubble up the ExecutionError.
                    raise
                    
        # Code path 1 (See docstring)
        return rpm_set
            

class ExtractPackage(Operation):
    '''
        Extract the contents of the package into the temp folder
    '''

    def __init__(self, gppkg):
        self.gppkg = gppkg 

    def execute(self):
        #clean up tmp extraction folder
        if os.path.exists(TEMP_EXTRACTION_PATH) and not CleanupDir(TEMP_EXTRACTION_PATH).run():
            logger.error('Could not clean temp folder')
            raise IOError

        #untar the package into tmp folder
        with closing(tarfile.open(self.gppkg.abspath)) as tarinfo:
            tarinfo.extractall(TEMP_EXTRACTION_PATH)
 
        #move all the deps into same folder as the main rpm
        path = os.path.join(TEMP_EXTRACTION_PATH, DEPS_DIR)
        if os.path.exists(path):
            for cur_file in os.listdir(path):
                shutil.move(os.path.join(TEMP_EXTRACTION_PATH, DEPS_DIR, cur_file), TEMP_EXTRACTION_PATH)



class InstallPackageLocally(Operation):
    """
    Installs a package on the local host

    This operation must take a slew of starting conditions and drive the state
    of the local machine towards the ending state, in which the given package is successfully
    installed, the rpm database is sane, and the package resides in the designated archive.

    We must consider the following scenarios and more: package was deleted from archive,
    the main comprising rpm was uninstalled, dependent rpms were removed, the rpm database was
    corrupted, etc.
    """
    def __init__(self, package_path):
        self.package_path = package_path
    def execute(self): 
        current_package_location = self.package_path
        package_name = os.path.basename(current_package_location)
        logger.info('Installing %s locally' % package_name)
        final_package_location = os.path.join(GPPKG_ARCHIVE_PATH, package_name)

        gppkg = Gppkg.from_package_path(current_package_location)
        ExtractPackage(gppkg).run()

        rpm_set = ValidateInstallRpms(gppkg).run()
        if not rpm_set:
            raise AlreadyInstalledError(gppkg.pkg)
        rpm_install_command = 'rpm -i %s --dbpath %s --prefix=%s' % \
                              (" ".join([os.path.join(TEMP_EXTRACTION_PATH, rpm) for rpm in rpm_set]), 
                               RPM_DATABASE, 
                               RPM_INSTALLATION_PATH)
        cmd = Command('Installing rpms', rpm_install_command)
        logger.info(cmd)
        cmd.run(validateAfter = True)

        # TODO: AK: Here, we are not being resilient to the package archive already
        # residing at final_package_location. This is technically not a bug, because
        # SyncPackages will not trigger an InstallPackageLocally if the package archive already exists. 
        # However, this assumption may not necessarily hold with later design improvements.
        MakeDir(GPPKG_ARCHIVE_PATH).run()
        shutil.copy(current_package_location, final_package_location)
        logger.info("Completed local installation of %s." % package_name)



class UnInstallSegGppkg(Operation):
    '''
        This Operation is invoked as a RemoteOperation. 
        It uninstalls the gppkg on the remote host.
    '''
    def __init__(self, pkg_name):
        self.pkg_name = pkg_name

    def execute(self): 
       
        rpm_uninstallation_command = 'rpm -e %s --dbpath %s ' 

        gppkg = Gppkg.from_package_path(os.path.join(GPPKG_ARCHIVE_PATH, self.pkg_name))

        #If RPM has not been installed then just return 
        if not IsRpmInstalled(gppkg.main_rpm).run():
            return 

        #Extract the package contents on the host
        ExtractPackage(gppkg).run()

        #Dependency check on the host
        #'-4' to remove the '.rpm' extension
        rpm_uninstall_string = gppkg.main_rpm[:-4] + ' ' 
        for dep in gppkg.dependencies:
            rpm_uninstall_string += dep[:-4] + ' '

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
                    rpm_uninstall_string = GetFilteredRpmString(results).run()          
                    break

        cmd_str = rpm_uninstallation_command % (rpm_uninstall_string, RPM_DATABASE)

        #run rpm -e on the host
        LocalCommand(cmd_str).run()        

class GetFilteredRpmString(Operation):
    """
    TODO: AK: This is poorly designed. The caller is responsible for 
    producing a very specific input as "results". More unfortunately, that 
    process for producing "results" is duplicated in a couple of places
    and it's fairly involved. This Operation should be taking care of that. 
    So, more explicitly, this class should be renamed to ValidateUninstallRpms,
    and it should follow the same design pattern as ValidateInstallRpms.

    Filters the results and returns those rpms which can be 
    uninstalled safely
    """
    def __init__(self, results, gppkg):
        self.results = results
        self.gppkg = gppkg

    def execute(self):

        results = self.results

        filtered_rpm_string = ' ' 
        dependencies_to_not_remove = set() 
        for result in results:
            tokens = result.split(' ')

            for token in tokens:
                if '.rpm' in token:
                    dependencies_to_not_remove.add(token.strip())

        if self.gppkg.main_rpm not in dependencies_to_not_remove:
            filtered_rpm_string = self.gppkg.main_rpm[:-4] + ' '

        for rpm in self.gppkg.dependencies:
            if rpm not in dependencies_to_not_remove:
                filtered_rpm_string += rpm[:-4] + ' '

        return filtered_rpm_string

class SyncPackages(Operation):
    """ Synchronizes packages from master to a remote host """
    def __init__(self, host):
        self.host = host
    def execute(self):
        if not CheckDir(GPPKG_ARCHIVE_PATH).run():
            MakeDir(GPPKG_ARCHIVE_PATH).run()
        if not CheckRemoteDir(GPPKG_ARCHIVE_PATH, self.host).run():
            MakeRemoteDir(GPPKG_ARCHIVE_PATH, self.host).run()

        # set of packages on the master
        master_package_set = set(ListFilesByPattern(GPPKG_ARCHIVE_PATH, '*' + GPPKG_EXTENSION).run())
        # set of packages on the remote host
        remote_package_set = set(ListRemoteFilesByPattern(GPPKG_ARCHIVE_PATH, '*' + GPPKG_EXTENSION, self.host).run())
        # packages to be uninstalled on the remote host             
        uninstall_package_set = remote_package_set - master_package_set
        # packages to be installed on the remote host
        install_package_set = master_package_set - remote_package_set

        if not install_package_set and not uninstall_package_set:
            logger.info('The packages on %s are consistent.' % self.host)
            return

        if install_package_set:
            logger.info('The following packages will be installed on %s: %s' % (self.host, ', '.join(install_package_set)))
            for package in install_package_set:
                logger.debug('copying %s to %s' % (package, self.host))
                dstFile = os.path.join(GPHOME, package)
                Scp(name = 'copying %s to %s' % (package, self.host), 
                    srcFile = os.path.join(GPPKG_ARCHIVE_PATH, package), 
                    dstFile = dstFile, 
                    dstHost = self.host).run(validateAfter = True)
                RemoteOperation(InstallPackageLocally(dstFile), self.host).run()
                RemoveRemoteFile(dstFile, self.host).run()

        if uninstall_package_set:
            logger.info('The following packages will be uninstalled on %s: %s' % (self.host, ', '.join(uninstall_package_set)))
            for package in uninstall_package_set:
                RemoteOperation(UnInstallSegGppkg(package), self.host).run()
                # TODO: AK: Unarchival should be the result of a succesful, local uninstallation! The UnArchive... should not be here.
                UnArchivePackageOnHost(self.host, package).run()

class InstallPackage(Operation):
    def __init__(self, gppkg, master_host, standby_host, segment_host_list):
        self.gppkg = gppkg
        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list

    def execute(self):
        logger.info('Installing package %s' % self.gppkg.pkg)

        #Check the GPDB requirements
        if not IsVersionCompatible(self.gppkg).run():
            raise GpdbVersionError

        # TODO: AK: I've changed our use of the OS tag from 'Linux' to 'rhel5' or 'suse10'.
        # So, the two lines below will not work properly.
        #if self.gppkg.os.lower() != platform.system().lower():
        #    raise OSCompatibilityError(self.gppkg.os, platform.system().lower())

        #architecture compatibility
        if self.gppkg.architecture.lower() !=  platform.machine().lower():
            raise ArchCompatibilityError(self.gppkg.architecture, platform.machine().lower())

        #check if already installed
        if IsRpmInstalled(self.gppkg.main_rpm).run():
            raise AlreadyInstalledError(self.gppkg.pkg)      # TODO: AK: self.gppkg.pkg is poorly named

        # perform any pre-installation steps 
        PerformHooks(hooks = self.gppkg.preinstall,
                     master_host = self.master_host,
                     standby_host = self.standby_host,
                     segment_host_list = self.segment_host_list).run() 

        # distribute package to segments
        srcFile = self.gppkg.abspath
        dstFile = os.path.join(GPHOME, self.gppkg.pkg)
        self.distribute_to_segments(srcFile, dstFile)

        # install package on segments
        self.install_on_segments(dstFile)

        # install package on standby
        if self.standby_host:
            Scp(name = 'copying %s to %s' % (srcFile, self.standby_host),
                srcFile = srcFile,
                dstFile = dstFile,
                dstHost = self.standby_host).run(validateAfter = True)
            RemoteOperation(InstallPackageLocally(dstFile), self.standby_host).run()
 
        # install package on master
        InstallPackageLocally(srcFile).run()

        # perform any post-installation steps 
        PerformHooks(hooks = self.gppkg.postinstall,
                     master_host = self.master_host,
                     standby_host = self.standby_host,
                     segment_host_list = self.segment_host_list).run() 

        logger.info('%s successfully installed.' % (self.gppkg.pkg))
    def distribute_to_segments(self, srcFile, dstFile):
        pool = WorkerPool()
        for host in self.segment_host_list:
            pool.addCommand(Scp(name = 'copying %s to %s' % (srcFile, host),
                                srcFile = srcFile,
                                dstFile = dstFile,
                                dstHost = host))
        pool.join()
    def install_on_segments(self, dstFile):
        """
        TODO: AK: With the proper support, this could be one line of code...
            SegmentOperation(InstallPackageLocally(dstFile))
        """
        operations = []
        for host in self.segment_host_list:
            operations.append(RemoteOperation(InstallPackageLocally(dstFile), host))
        ParallelOperation(operations).run()
        for operation in operations:
            operation.get_ret()



class PerformHooks(Operation):
    def __init__(self, hooks, master_host, standby_host, segment_host_list):
        """
        Performs steps that have been specified in the yaml file for a particular
        stage of gppkg execution

        TODO: AK: A packager may have added commands to their hooks, with the 
        assumption that the current working directory would be that which contains
        the spec file, rpms, and other artifacts (external scripts, perhaps.) To support
        this, these commands should be prefixed with a "cd".

        TODO: AK: I'm adding master_host for consistency. 
        But, why would we ever need master_host?  We're on the master host!
        """
        self.hooks = hooks
        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list
    def execute(self):
        if self.hooks is None:
            return
        for hook in self.hooks:
            key = hook.keys()
            if key is None:
                return
            key_str = key[0]
            if key_str.lower() == 'master':
                if self.standby_host:
                    RemoteCommand(hook[key_str], [self.standby_host]).run()
                LocalCommand(hook[key_str], True).run()
            elif key_str.lower() == 'segment':
                RemoteCommand(hook[key_str], self.segment_host_list).run()

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
        with closing(tarfile.open(gppkg.abspath)) as tarinfo:
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
                    rpm_uninstall_string = GetFilteredRpmString(results).run()            
                    break

        cmd_str = 'rpm -e %s --dbpath %s' % (rpm_uninstall_string, RPM_DATABASE)

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

        #UnArchive packages from hosts
        for host in self.segment_host_list:
            UnArchivePackageOnHost(host, gppkg.pkg).run()
        
        logger.info('%s has been uninstalled.' % (gppkg.pkg))

    def _perform_pre_uninstall(self, gppkg):
        """
        Perform pre uninstall steps that have been specified in yml file

        TODO: AK: A packager may have added commands to their hooks, with the 
        assumption that the current working directory would be that which contains
        the spec file, rpms, and other artifacts (external scripts, perhaps.) To support
        this, these commands should be prefixed with a "cd".
        """
        logger.debug('_perform_pre_uninstall')

        if gppkg.preuninstall is None:
            logger.info('No preuninstall hooks')
            return 

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
            
    def _perform_post_uninstall(self, gppkg):
        """
        Perform post uninstall steps that have been specified in yml file
        
        TODO: AK: A packager may have added commands to their hooks, with the 
        assumption that the current working directory would be that which contains
        the spec file, rpms, and other artifacts. (external scripts, perhaps.) To support
        this, these commands should be prefixed with a "cd".
        """
        logger.debug('_perform_post_uninstall')

        if gppkg.postuninstall is None:
            logger.info('No postuninstall hooks')
            return 

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
            print 'GPDBVersion:    %s' % gppkg.gpdbversion
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
        pkg = pkg_path_details['pkgname'] + '-' + str(pkg_path_details['version']) + '-' + pkg_path_details['os'] + '-' + pkg_path_details['architecture'] + GPPKG_EXTENSION 
        if os.path.exists(pkg):
            os.remove(pkg)

        #Verify the spec file 
        if not self._verify_specfile(specfile, directory):
            raise BuildPkgError 

        #tar and gzip the directory
        #rename the file with .gppkg extension
        with closing(tarfile.open(pkg, 'w:gz')) as tarinfo:
            for cur_file in filelist:
                tarinfo.add(name = os.path.join(directory, cur_file),
                            arcname = cur_file)

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
        gppkg = self.gppkg

        #Check version compatibility
        if not IsVersionCompatible(gppkg).run():
            raise GpdbVersionError

        #Extract the contents of the package into a temp folder
        ExtractPackage(gppkg).run()

        #Transfer package
        TransferPackageContents(self.master_host, self.standby_host, self.segment_host_list).run() 

        #get the installation string
        rpm_set = ValidateInstallRpms(gppkg).run()
        if not rpm_set:
            raise AlreadyInstalledError(gppkg.pkg)
        rpm_install_string = " ".join([os.path.join(TEMP_EXTRACTION_PATH, rpm) for rpm in rpm_set])

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
        
        cmd_str = 'rpm -U %s --dbpath %s --prefix=%s' % (rpm_install_string, RPM_DATABASE, RPM_INSTALLATION_PATH)

        #first the segments
        RemoteCommand(cmd_str, self.segment_host_list).run()

        #next standby
        if self.standby_host:
            RemoteCommand(cmd_str, [self.standby_host]).run()

        #last master
        LocalCommand(cmd_str).run()

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

    def __init__(self, standby_host, segment_host_list):
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list

    def execute(self):
        operations = [SyncPackages(host) for host in self.segment_host_list]
        
        if self.standby_host:
            operations.append(SyncPackages(self.standby_host))
        
        ParallelOperation(operations).run()  

        for operation in operations:
            try:
                operation.get_ret() 
            except Exception, e:
                raise ExceptionNoStackTraceNeeded('SyncPackages failed' + str(e))      

        logger.info('Successfully cleaned the cluster')



class MigratePackages(Operation):
    """
    Migrates packages from another $GPHOME to this one

    This functionality is meant to facilitate minor version upgrade, whereby old packages
    need to be brought over from the older $GPHOME to the newer $GPHOME.

    Presumably, this could also be used to migrate packages across arbitrary choices
    of $GPHOMEs. However, the migration will only succeed if the packages being migrated
    are actually compatible with the target GPDB.
    """
    def __init__(self, from_gphome, to_gphome):
        self.from_gphome, self.to_gphome = from_gphome, to_gphome
    def execute(self):
        if not os.path.samefile(self.to_gphome, GPHOME):
            raise ExceptionNoStackTraceNeeded('The target GPHOME, %s, must match the current $GPHOME used to launch gppkg.' % self.to_gphome)
        if os.path.samefile(self.to_gphome, self.from_gphome):
            raise ExceptionNoStackTraceNeeded('The source and target GPHOMEs, %s => %s, must differ for packages to be migrated.' % (self.from_gphome, self.to_gphome))

        # TODO: AK: Given an invalid from_gphome, we'll end up creating a 'share/packages' subdirectory within it.
        old_archive_path = os.path.join(self.from_gphome, ARCHIVE_PATH)
        MakeDir(old_archive_path).run()
        packages = ListFilesByPattern(old_archive_path, '*' + GPPKG_EXTENSION).run()
        if not packages:
            logger.info('There are no packages to migrate from %s.' % self.from_gphome)
            return

        logger.info('The following packages will be migrated: %s' % ', '.join(packages))
        for package in packages:
            package_path = os.path.join(old_archive_path, package)
            try:
                InstallPackageLocally(package_path).run()
            except AlreadyInstalledError:
                logger.info("%s is already installed." % package)
            except Exception:
                logger.exception("Failed to migrate %s from %s" % (old_archive_path, package))
        logger.info('The package migration has completed.')
