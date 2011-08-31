#!/usr/bin/env python
#
# Line too long - pylint: disable=C0301
# Copyright (c) Greenplum Inc 2010. All Rights Reserved.
#
# import mainUtils FIRST to get python version check
# THIS IMPORT SHOULD COME FIRST

try:
    import tarfile
    import yaml
    import os, sys 
    from gppylib.mainUtils import addMasterDirectoryOptionForSingleClusterProgram, addStandardLoggingAndHelpOptions
    from gppylib.commands import gp
    from gppylib.db import dbconn
    from optparse import OptionGroup 
    from gppylib.gpparseopts import OptParser, OptChecker
    from gppylib import gplog
    from gppylib import pgconf
    from gppylib.gparray import GpArray
    from gppylib.operations.package import SPECFILE_NAME, InstallGppkg, UnInstallGppkg, QueryGppkg, BuildGppkg, UpdateGppkg, CleanGppkg
    from gppylib.gpversion import GpVersion

except ImportError, ex:
    sys.exit('Cannot import modules.  Please check that you have sourced greenplum_path.sh.  Detail: ' + str(ex))

logger = gplog.get_default_logger()

class Gppkg:

    '''
        This class stores all the information about a gppkg
    '''
                
    def __init__(self, pkg, pkgname, main_rpm, version, architecture, os, gpdbmin, gpdbmax, description, abspath, preinstall, postinstall, preuninstall, postuninstall, dependencies, file_list):

        logger.debug('Gppkg Constructor')

        self.pkg = pkg          #This stores the complete package name E.g pgcrypto-1.0-Darwin-i386.gppkg
        self.pkgname = pkgname  #This only store the name of the package as specified in the spec file
        self.main_rpm = main_rpm
        self.version = version 
        self.architecture = architecture
        self.os = os
        self.gpdbmin = gpdbmin
        self.gpdbmax = gpdbmax
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
           
        tarinfo = None

        if not os.path.exists(pkg_path):
            logger.error('Cannot find package %s' % pkg_path)
            raise IOError 

        #We check for a directory first because 
        #is_tarfile does not accept directories as path names
        if os.path.isdir(pkg_path):
            logger.error('%s is a directory !' % pkg_path)
            raise IOError 

        if not tarfile.is_tarfile(pkg_path) or not pkg_path.endswith('.gppkg'):
            logger.error('%s is Not a valid package' % pkg_path)
            raise IOError 

        if os.path.getsize(pkg_path) == 0:
            logger.error('Package is empty')
            raise IOError 

        pkg = {'pkg': '', 'pkgname':'', 'os':'', 'architecture':'', 'gpdbmin':'', 'gpdbmax':'', 'description':'', 
        'preinstall' : [], 'postinstall' : [], 'preuninstall' : [], 'postuninstall' : [], 'file_list' : [], 'dependencies' : [] } 
    
        try:
            tarinfo = tarfile.open(pkg_path, 'r:gz')
            archive_list = tarinfo.getnames()

            #store the list of all files present in the archive
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

        finally:
            if tarinfo:
                tarinfo.close()

        #store all the tags
        for key in keys:
            pkg[key.lower()] = yamlfile[key]    
        
        #update the pkgpath
        pkg['pkg'] = os.path.split(pkg_path)[-1] 

        #make the versions as strings
        pkg['version'] = str(pkg['version'])

        pkg['gpdbmin'] = str(pkg['gpdbmin'])
        #TODO: add 0s to the end to make the version of the form a.b.c.d
        version_list = pkg['gpdbmin'].split('.')
        while len(version_list) != 4:
            version_list.append('0')
        #Convert it to GpVersion
        pkg['gpdbmin'] = GpVersion('.'.join(version_list))

        pkg['gpdbmax'] = str(pkg['gpdbmax'])
        version_list = pkg['gpdbmax'].split('.')
        while len(version_list) != 4:
            version_list.append('0')
        #Convert it to GpVersion
        pkg['gpdbmax'] = GpVersion('.'.join(version_list))

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

class GpPkgProgram:

    '''
        This class is the main class for gppkg tool. 
        This class contains the methods to install,
        remove, update packages.
    '''
    
    def __init__(self, options, args):

        logger.debug('GpPkgProgram Constructor')

        self.__options = options
        self.__args = args
        if options.masterDataDirectory is None:
            options.masterDataDirectory  = gp.get_masterdatadir()        
        self.master_datadir = options.masterDataDirectory 
        self.master_port = self._get_master_port(self.master_datadir)
        self.master_host = None
        self.standby_host = None
        self.segment_host_list = None
        self.version = None
        self.gphome = None
        self.query = options.query
        self.args = args
        self.build = options.build
        self.install = options.install
        self.remove = options.remove
        self.update = options.update
        self.clean = options.clean

    @staticmethod
    def create_parser():

        '''
             This method creates a parser object and returns it to the caller
        '''

        logger.debug('create_parser')

        parser = OptParser(option_class=OptChecker,
            description="Install/Update/Remove a gppkg.",
            version='%prog version $Revision: #1 $')
        parser.setHelp([])

        addStandardLoggingAndHelpOptions(parser, includeNonInteractiveOption=True)

        parser.remove_option('-q')
        parser.remove_option('-l')
        
        add_to = OptionGroup(parser, 'General Options')
        parser.add_option_group(add_to)

        addMasterDirectoryOptionForSingleClusterProgram(add_to)

        add_to.add_option('-c', '--clean', help='clean the cluster of the given gppkg', metavar='<package>')
        add_to.add_option('-i', '--install', help='install the given gppkg', metavar='<package>')
        add_to.add_option('-u', '--update', help='update the given gppkg', metavar='<package>')
        add_to.add_option('-r', '--remove', help='remove the given gppkg', metavar='<package>')
        add_to.add_option('-b', '--build', help='build a gppkg', metavar='<directory>')
        add_to.add_option('-q', '--query', help='query the given gppkg', metavar='<package>')
        #add_to.add_option('--query', help='query the given gppkg', metavar='<package>')

        add_to = OptionGroup(parser, 'Query Options')
        parser.add_option_group(add_to)
        add_to.add_option('-I', '--info', help='print information about the gppkg including name, version, description')
        add_to.add_option('-R', '--requires', help='print the dependencies of the gppkg')
        add_to.add_option('-l', '--list', help='print all the files present in the gppkg')
        add_to.add_option('--all', help='print all the gppkgs installed by gppkg')
        #add_to.add_option('--list', help='print all the files present in the gppkg')

        return parser
            
    @staticmethod
    def create_program(options, args):

        '''
            Starts the program
        '''

        logger.debug('create_program')
        return GpPkgProgram(options, args)    

    def _get_gpdb_host_list(self):
        
        ''' 
            This method gets the host names 
            of all hosts in the gpdb array.
            It sets the following variables 
                GpPkgProgram.master_host to master
                GpPkgProgram.standby_host to standby
                GpPkgProgram.segment_host_list to segment hosts
        '''
        
        logger.debug('_get_dpdb_host_list')
        
        #Get host list
        gparr = GpArray.initFromCatalog(dbconn.DbURL(port = self.master_port), utility = True)
        master_host = None
        standby_host = None 
        segment_host_list = []

        segs = gparr.getDbList()

        for seg in segs:
            if seg.isSegmentMaster(current_role = True):
                master_host = seg.getSegmentHostName()
            elif seg.isSegmentStandby(current_role = True):
                standby_host = seg.getSegmentHostName()
            else:
                segment_host_list.append(seg.getSegmentHostName())

        #Deduplicate the hosts so that we 
        #dont install multiple times on the same host
        segment_host_list = list(set(segment_host_list))

        #Segments might exist on the master host. Since we store the 
        #master host separately in self.master_host, storing the master_host
        #in the segment_host_list is redundant.
        for host in segment_host_list:
            if host == master_host or host == standby_host:
                segment_host_list.remove(host)

        self.master_host = master_host
        self.standby_host = standby_host
        self.segment_host_list = segment_host_list

    def _get_master_port(self, datadir):
       
        '''
            Obtain the master port from the pgconf file
        '''
       
        logger.debug('_get_master_port')
        pgconf_dict = pgconf.readfile(os.path.join(datadir, 'postgresql.conf'))
        return pgconf_dict.int('port')

    def run(self):

        if self.build:
            BuildGppkg(self.build).run()    
            return 

        #Get the host list first
        self._get_gpdb_host_list()

        if self.install:
            pkg = Gppkg.from_package_path(self.install)
            InstallGppkg(pkg, self.master_host, self.standby_host, self.segment_host_list).run() 
        elif self.query:
            try:
                if len(self.__args) == 0:
                    if self.query not in ['all', '--all']:
                        pkg = Gppkg.from_package_path(self.query)
                    else:
                        pkg = None
                else:
                    pkg = Gppkg.from_package_path(self.__args[0])
            except KeyError, ex:
                print 'Missing key %s' % str(ex)

            if self.query in ['I', 'info', 'R', 'requires', 'l', 'list', '--info', '--list', '--requires', '-I', '-R', '-l', 'all', '--all']:
                QueryGppkg(pkg, self.query).run()
            elif os.path.isfile(self.query):
                QueryGppkg(pkg, self.query).run()
            else:
                logger.error('Invalid option %s' % self.query)
        elif self.remove:
            pkg = Gppkg.from_package_path(self.remove)
            UnInstallGppkg(pkg, self.master_host, self.standby_host, self.segment_host_list).run()
        elif self.update:
            pkg = Gppkg.from_package_path(self.update)
            UpdateGppkg(pkg, self.master_host, self.standby_host, self.segment_host_list).run()
        elif self.clean:
            pkg = Gppkg.from_package_path(self.clean)
            CleanGppkg(pkg, self.master_host, self.standby_host, self.segment_host_list).run()

    def cleanup(self):
       
        '''  
            Clean up before exiting
        '''
