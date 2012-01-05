#!/usr/bin/env python

import os
import shutil
import unittest2 as unittest
import tempfile
import platform
import getpass
import tarfile
 
from gppylib.db import dbconn
from gppylib.gparray import GpArray
from contextlib import closing
from gppylib.commands import gp
from gppylib.commands.unix import Scp
from gppylib.commands.base import Command, ExecutionError, REMOTE
from gppylib.operations import Operation
from gppylib.operations.unix import CheckFile, CheckRemoteFile, RemoveRemoteFile
from gppylib.operations.package import dereference_symlink, GpScp

OS = platform.system()
ARCH = platform.machine()

# AK: use dereference_symlink when mucking with RPM database for the same reason 
# it's used in the gppylib.operations.package. For more info, see the function definition.
GPHOME = dereference_symlink(gp.get_gphome())

ARCHIVE_PATH = os.path.join(GPHOME, 'share/packages/archive')
RPM_DATABASE = os.path.join(GPHOME, 'share/packages/database') 
GPPKG_EXTENSION = ".gppkg"
SCRATCH_SPACE = os.path.join(tempfile.gettempdir(), getpass.getuser())
GPDB_VERSION = "4.2"
MASTER_PORT = os.getenv("PGPORT")

def requiresSpecialGphome():
    """
    A decorator which skips a unit test if GPHOME is 
    named as "greenplum-db" or if a symlink cannot be 
    created due to write permissions.
    """
    
    gp_home = os.getenv('GPHOME')
    if not gp_home.endswith('/.'):
        if os.path.split(gp_home)[1] == "greenplum-db":
            return unittest.skip("cannot create symlink")

    try:
        tmp_dir = os.path.join(os.path.dirname(GPHOME), "tmp")
        os.mkdir(tmp_dir)
        os.rmdir(tmp_dir)
    except OSError, e:
       return unittest.skip("cannot create symlink") 

    return lambda o: o

def skipIfNoStandby():
    """
    A decorator which skips a unit test if a standby
    is not already present in the cluster.
    """
    standby = get_host_list()[0]
    if standby is None:
        return unittest.skip("requires standby") 
    return lambda o: o 

def get_host_list():
    """
    Returns a tuple which consists of the standby
    and segment hosts comprising the cluster.

    @return: tuple containing the standby and segment hosts
             tuple[0] contains standby
             tuple[1] contains segment hosts
    """
    gparr = GpArray.initFromCatalog(dbconn.DbURL(port = MASTER_PORT), utility = True)
    segs = gparr.getDbList()

    standby_host = None
    segment_host_list = []

    for seg in segs:
        if seg.isSegmentStandby(current_role = True):
            standby_host = seg.getSegmentHostName()
        elif not seg.isSegmentMaster(current_role = True):
            segment_host_list.append(seg.getSegmentHostName())

    #Deduplicate the hosts so that we 
    #dont install multiple times on the same host
    segment_host_list = list(set(segment_host_list))

    return (standby_host, segment_host_list)

def run_command(cmd_str):
    """
    Runs a command on the localhost
       
    @param cmd_str: The command string to be executed 
    @return: stdout/stderr output as a string 
    """
    cmd = Command("Local Command", cmd_str)
    cmd.run(validateAfter = True)
    results = cmd.get_results()
    
    if results.rc != 0:
        return results.stderr.strip()
    else:
        return results.stdout.strip()

def run_remote_command(cmd_str, host):
    """
    Runs a command on a remote host
    
    @param cmd_str: The command string to be executed
    @return: stdout/stderr output as a string
    """
    cmd = Command(name = "Remote Command",
                  cmdStr = cmd_str,
                  ctxt = REMOTE,
                  remoteHost = host)
    cmd.run(validateAfter = True)
    
    results = cmd.get_results()
    
    if results.rc != 0:
        return results.stderr.strip()
    else:
        return results.stdout.strip()

class RPMSpec:
    """Represents an RPM spec file used for creating an RPM"""
    def __init__(self, name, version, release, depends = []):
        """ 
        @param depends: List of dependecies for the rpm
        @type depends: List of strings
        """
        self.name = name
        self.version = version
        self.release = release
        self.depends = depends

    def get_package_name(self):
        """Returns the package name of the form <name>-<version>-<release>"""
        return self.name + '-' + self.version + '-' + self.release 

    def get_filename(self):
        """Returns the complete filename of the rpm""" 
        return self.get_package_name() + '.' + ARCH + ".rpm"

    def __str__(self):
        """Returns the rpm spec file as a string"""
        rpm_spec_file = '''

%define _topdir %(pwd)
%define __os_install_post %{nil}

Summary:        Temporary test package
License:        GPLv2        
Name:           ''' + self.name + '''          
Version:        ''' + self.version + '''
Release:        ''' + self.release + ''' 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      ''' + ARCH + ''' 
Provides:       ''' + self.name + ''' = '''+ self.version +''', /bin/sh
BuildRoot:      %{_topdir}/BUILD '''

        if self.depends != []: 
            rpm_spec_file += '''
Requires:       ''' + ','.join(self.depends)
        
        rpm_spec_file += '''
%description
Temporary test package for gppkg.

%install
mkdir -p %{buildroot}/temp
touch %{buildroot}/temp/temp_file

%post

%postun

%files
/temp
'''
        return rpm_spec_file

class BuildRPM(Operation):
    def __init__(self, spec):
        """
        @param spec: The spec file required to build the rpm
        @type spec: RPMSpec
        """
        self.spec = spec

    def execute(self):
        rpm_spec_file = str(self.spec)
        build_dir = os.path.join(SCRATCH_SPACE, "BUILD") 
        os.mkdir(build_dir)
        rpms_dir = os.path.join(SCRATCH_SPACE, "RPMS")
        os.mkdir(rpms_dir)

        try:
            with tempfile.NamedTemporaryFile() as f:
                f.write(rpm_spec_file)
                f.flush()
            
                os.system("cd " + SCRATCH_SPACE + "; rpmbuild --quiet -bb " + f.name)

            shutil.copy(os.path.join(SCRATCH_SPACE, "RPMS", ARCH, self.spec.get_filename()), os.getcwd())
        finally:
            shutil.rmtree(build_dir)
            shutil.rmtree(rpms_dir)

        return self.spec.get_filename() 

class GppkgSpec:
    """Represents the gppkg spec file"""
    def __init__(self, name, version, gpdbversion = GPDB_VERSION, os = OS, arch = ARCH):
        """
        All the parameters require arguments of type string.
        """
        self.name = name 
        self.version = version
        self.gpdbversion = gpdbversion
        self.os = os
        self.arch = arch

    def get_package_name(self):
        """Returns the package name of the form <name>-<version>"""
        return self.name + '-' + self.version       

    def get_filename(self):
        """Returns the complete filename of the gppkg"""
        return self.get_package_name() + '-' + self.os + '-' + self.arch + GPPKG_EXTENSION

    def __str__(self): 
        """Returns the GppkgSpec in the form of a string"""
        gppkg_spec_file = '''
PkgName: ''' + self.name + '''
Version: ''' + self.version + '''
GPDBVersion: ''' + self.gpdbversion + '''
Description: Temporary Test Package
OS: ''' + self.os + '''
Architecture: ''' + self.arch 

        return gppkg_spec_file

class BuildGppkg(Operation):
    def __init__(self, gppkg_spec, main_rpm_spec, dependent_rpm_specs = []):
        """
        @param gppkg_spec: The spec file required to build the Gppkg
        @type gppkg_spec: GppkgSpec
        @param main_rpm_spec: The spec file required to build the main rpm
        @type main_rpm_spec: RPMSpec
        @param dependent_rpm_specs: The list of all dependent rpm specs
        @type dependent_rpm_specs: List of RPMSpec
        """
        self.gppkg_spec = gppkg_spec
        self.main_rpm_spec = main_rpm_spec
        self.dependent_rpm_specs = dependent_rpm_specs

    def execute(self):

        if not os.path.exists(SCRATCH_SPACE):
            os.mkdir(SCRATCH_SPACE)
        
        gppkg_spec_file = str(self.gppkg_spec)
        #create gppkg_dir
        gppkg_dir = os.path.join(SCRATCH_SPACE, "package")
        if not os.path.exists(gppkg_dir):
            os.mkdir(gppkg_dir)
           
        dependent_rpms = [rpm.get_package_name() for rpm in self.dependent_rpm_specs] 
        rpm_file = BuildRPM(self.main_rpm_spec).run()    
        shutil.move(rpm_file, gppkg_dir)

        try:
            if self.dependent_rpm_specs != []:
                deps_dir = os.path.join(gppkg_dir, "deps")
                os.mkdir(deps_dir)

                for spec in self.dependent_rpm_specs:
                    rpm_file = BuildRPM(spec).run()
                    shutil.move(rpm_file, deps_dir)
        
            #create gppkg
            with open(os.path.join(gppkg_dir, "gppkg_spec.yml"), "w") as f:
                f.write(gppkg_spec_file)

            run_command("gppkg --build " + gppkg_dir)
        finally:
            shutil.rmtree(gppkg_dir)
            shutil.rmtree(SCRATCH_SPACE)

        return self.gppkg_spec.get_filename()

class GppkgTestCase(unittest.TestCase):
    """
    Basic gppkg test case from which all the other test cases inherit.

    Provides default RPMSpec and GppkgSpecs for the inherited classes
    to work with.
    Default RPMSpec will have no dependencies, have a version and release 1 
    and will be called test
    Default GppkgSpec will have no dependencies, have version 1.0 and will 
    be called test
    """
    def setUp(self):
        self.cleanup()
        self.A_spec = RPMSpec("A", "1", "1")
        self.alpha_spec = GppkgSpec("alpha", "1.0")

    def tearDown(self): 
        self.cleanup()
   
    @classmethod 
    def setUpClass(self):
        self.extra_clean = set()
        self.start_output, self.end_output = (1, None) if os.getenv('USE_FIGLEAF', None) is None else (2, -2)
    
    @classmethod
    def tearDownClass(self):
        for gppkg in self.extra_clean:
            os.remove(gppkg)

    def cleanup(self):
        """Cleans up gppkgs that are installed"""
        results = run_command("gppkg -q --all")
        gppkgs = results.split('\n')[self.start_output:self.end_output]   #The first line is 'Starting gppkg with args', which we want to ignore. 

        for gppkg in gppkgs:
            run_command("gppkg --remove " + gppkg)

    def build(self, gppkg_spec, rpm_spec, dep_rpm_specs = []):
        """
        Builds a gppkg and checks if the build was successful.
        if the build was successful, returns the gppkg filename
        
        @param gppkg_spec: The spec file required to build a gppkg
        @type gppkg_spec: GppkgSpec
        @param rpm_spec: The spec file required to build the main rpm
        @type rpm_spec: RPMSpec
        @param dep_rpm_specs: List of all the dependecies of the main rpm
        @type dep_rpm_specs: List of RPMSpecs
        @return: gppkg filename
        @rtype: string
        """
        gppkg_file = BuildGppkg(gppkg_spec, rpm_spec, dep_rpm_specs).run()
        assert self._check_build(gppkg_file, gppkg_spec)
        self.extra_clean.add(gppkg_file)
        return gppkg_file
        
    def _check_build(self, gppkg_file, gppkg_spec):
        """
        Check if the gppkg build was successful
        
        @return: True if build was successful
                 False otherwise
        @rtype: bool
        """
        return gppkg_file == gppkg_spec.get_filename()
        
    def install(self, gppkg_filename):
        """
        Install a given gppkg and checks if the installation was 
        successful.

        @param gppkg_filename: The name of the gppkg file
        @type gppkg_filename: str
        """
        run_command("gppkg --install %s" % gppkg_filename)
        assert self.check_install(gppkg_filename)
      
    def check_install(self, gppkg_filename):
        """
        Checks if a gppkg was installed successfuly.
        
        @param gppkg_filename: The name of the gppkg file
        @type gppkg_filename: str
        @return: True if the gppkg was installed successfully
                 False otherwise
        @rtype: bool
        """
        cmd = "gppkg -q %s" % gppkg_filename
        results = run_command(cmd)
        test_str = ''.join(gppkg_filename.split('-')[:1]) + " is installed" 
        is_installed = test_str in results 
        return is_installed and CheckFile(os.path.join(ARCHIVE_PATH, gppkg_filename)).run()

    def remove(self, gppkg_filename):
        """
        Uninstalls a gppkg and checks if the uninstallation was
        successful.

        @param gppkg_filename: The name of the gppkg file to be uninstalled
        @type gppkg_filename: str
        """
        gppkg_package_name = gppkg_filename.split('-')[0] + '-' + gppkg_filename.split('-')[1]
        run_command("gppkg --remove %s" % gppkg_package_name)
        assert not self.check_install(gppkg_filename)

    def update(self, gppkg_filename):
        """
        Updates a gppkg and checks if the update was 
        successful.

        @param gppkg_filename: The name of the gppkg to be updated
        @type gppkg_filename: str
        """
        run_command("gppkg --update %s" % gppkg_filename)
        assert self.check_install(gppkg_filename)

    def remove_timestamp(self, result):
        """
        Removes timestamp from the output produced by gppkg.

        The output produced by gppkg will be of the form
        [timestamp] gppkg:host:user[INFO]:- Some message
        We want to strip out the timestamp and other unwanted
        text such as [INFO], gppkg, host, user etc.
        
        @param result: The output from stdout/stderr
        @type result: str
        @return: The output with the timestamps and unwanted text removed
        @rtype: str
        """
        result_without_timestamp = []
        results = result.split('\n')[self.start_output:self.end_output]
    
        for res in results:
            res = res.split(':-')[1].strip()
            res = ''.join(res.split(' '))
            result_without_timestamp.append(res)

        return result_without_timestamp 

    def check_rpm_install(self, rpm_package_name):
        """
        Checks if an rpm has been installed or not.

        @param rpm_package_name: Name of the rpm package of the form <name>-<version>-<release> 
        @type rpm_package_name: str
        """
        results = run_command("rpm -q %s --dbpath %s" % (rpm_package_name, RPM_DATABASE))
        assert results == rpm_package_name

    def check_rpm_uninstall(self, rpm_package_name):
        """
        Checks if an rpm has been uninstalled successfuly.

        @param rpm_package_name: Name of rpm package of the form <name>-<version>-<release>
        @type rpm_package_name: str
        """
        with self.assertRaisesRegexp(ExecutionError, "%s is not installed" % rpm_package_name):
            run_command("rpm -q %s --dbpath %s" % (rpm_package_name, RPM_DATABASE))

    def check_remote_rpm_install(self, rpm_package_name, host):
        """
        Checks if an rpm has been installed on a remote host

        @param rpm_package_name: Name of rpm package of the form <name>-<version>-<release>
        @type rpm_package_name: str
        @param host: Remote host 
        @type host: str
        """
        results = run_remote_command("rpm -q %s --dbpath %s" % (rpm_package_name, RPM_DATABASE), host)
        assert results == rpm_package_name

    def check_remote_rpm_uninstall(self, rpm_package_name, host):
        """
        Checks if an rpm has been uninstalled on a remote host

        @param rpm_package_name: Name of rpm package of the form <name>-<version>-<release>
        @type rpm_package_name: str
        @param host: Remote host 
        @type host: str
        """
        with self.assertRaisesRegexp(ExecutionError, "%s is not installed" % rpm_package_name):
            results = run_remote_command("rpm -q %s --dbpath %s" % (rpm_package_name, RPM_DATABASE), host)
        
    def install_rpm(self, rpm_filename, rpm_database = RPM_DATABASE, installation_prefix = GPHOME):
        """
        Installs a given rpm and checks if the installation was successful.
        
        @param rpm_filename: Name of the rpm file to be installed
        @type rpm_filename: str
        @param rpm_database: The rpm database against which rpms will be installed 
        @type rpm_database: str
        @param installation_prefix: The installation path for the rpm 
        @param type installation_prefix: str
        """
        run_command("rpm -i %s --dbpath %s --prefix=%s" % (rpm_filename, rpm_database, installation_prefix))
        rpm_package_name = rpm_filename[:rpm_filename.index('.')] 
        self.check_rpm_install(rpm_package_name)

    def uninstall_rpm(self, rpm_filename, rpm_database = RPM_DATABASE):
        """
        UnInstalls a given rpm and checks if the unsintallation was successful

        @param rpm_filename: Name of the rpm file to be uninstalled
        @type rpm_filename: str
        @param rpm_database: The rpm database against which rpms will be uninstalled 
        @type rpm_database: str
        """
        rpm_package_name = rpm_filename[:rpm_filename.index('.')]
        run_command("rpm -e %s --dbpath %s" % (rpm_package_name, rpm_database))
        self.check_rpm_uninstall(rpm_package_name) 

    def install_rpm_remotely(self, rpm_filename, host, rpm_database = RPM_DATABASE, installation_prefix = GPHOME):
        """
        Installs an rpm on a remote host and checks if the installation was successful

        @param rpm_filename: Name of the rpm file to be installed
        @type rpm_filename: str
        @param host: The remote host on which the rpm will be installed
        @type host: str
        @param rpm_database: The rpm database against which rpms will be installed
        @type rpm_database: str
        @param installation_prefix: The installation path for the rpm
        @type installation_prefix: str
        """
        if host is None:
            return
        run_remote_command("rpm -i %s --dbpath %s --prefix=%s" % (rpm_filename, rpm_database, installation_prefix), host)
        rpm_package_name = rpm_filename[:rpm_filename.index('.')]
        self.check_remote_rpm_install(rpm_package_name, host)

    def uninstall_rpm_remotely(self, rpm_filename, host, rpm_database = RPM_DATABASE):
        """
        Uninstalls an rpm on a remote host and checks if uninstallation was successful

        @param rpm_filename: Name of the rpm file to be uninstalled
        @type rpm_filename: str
        @param host: The remote host on which the rpm will be uninstalled
        @type host: str
        @param rpm_database: The rpm database against which rpms will be uninstalled
        @type rpm_database: str
        """
        if host is None:
            return
        rpm_package_name = rpm_filename[:rpm_filename.index('.')]
        run_remote_command("rpm -e %s --dbpath %s" % (rpm_package_name, rpm_database), host)
        self.check_remote_rpm_uninstall(rpm_package_name, host)

class SimpleGppkgTestCase(GppkgTestCase):
    """Covers simple build/install/remove/update test cases"""
    def test00_simple_build(self):
        self.build(self.alpha_spec, self.A_spec)

    def test01_simple_install(self):
        gppkg_file = self.alpha_spec.get_filename() 
        self.install(gppkg_file)

        #Check RPM database
        self.check_rpm_install(self.A_spec.get_package_name())

    def test02_simple_update(self):
        gppkg_file = self.alpha_spec.get_filename() 
        self.install(gppkg_file)

        update_rpm_spec = RPMSpec("A", "1", "2")
        update_gppkg_spec = GppkgSpec("beta", "1.1")
        update_gppkg_file = self.build(update_gppkg_spec, update_rpm_spec)
   
        self.update(update_gppkg_file)

        #Check for the packages
        self.check_rpm_install(update_rpm_spec.get_package_name())

    def test03_simple_uninstall(self):
        gppkg_file = self.alpha_spec.get_filename()

        self.install(gppkg_file)
        self.remove(gppkg_file)

        results = run_command("gppkg -q --all")
        results = results.split('\n')[self.start_output:self.end_output]
        
        assert results == []
       
    def test04_help(self):
        help_options = ["--help", "-h", "-?"] 

        for opt in help_options:
            results = run_command("gppkg " + opt)
            assert results != ""

    def test05_version(self):
        results = run_command("gppkg --version")
        assert results != ""

class QueryTestCase(GppkgTestCase):
    """Test the query option of gppkg"""
    def setUp(self):
        self.A_spec = RPMSpec("A", "1", "1")    
        self.alpha_spec = GppkgSpec("alpha", "1.0")
 
        self.B_spec = RPMSpec("B", "1", "1")
        self.beta_spec = GppkgSpec("beta", "1.0")

    def tearDown(self):
        '''
            Overriding the teardown here as successive tests
            in the suite make use of gppkg installed in the
            previous test.
        '''
        pass

    def test00_install(self):
        gppkg_file1 = self.build(self.alpha_spec, self.A_spec)
        gppkg_file2 = self.build(self.beta_spec, self.B_spec)

        self.install(gppkg_file1) 
        self.install(gppkg_file2)       
  
    def test01_query_all(self):
        
        results = run_command("gppkg -q --all")
        assert results.split('\n')[self.start_output:self.end_output] == [self.alpha_spec.get_package_name(), self.beta_spec.get_package_name()]

        results = run_command("gppkg --all -q")
        assert results.split('\n')[self.start_output:self.end_output], [self.alpha_spec.get_package_name(), self.beta_spec.get_package_name()]

        self.assertRaises(ExecutionError, run_command, "gppkg -qall")

    def test02_query_info(self):
        expected_info_result = ['Namealpha', 'Version1.0', 'Architecturex86_64', 'OSLinux', 'GPDBVersionmainbuilddev', 'DescriptionTemporaryTestPackage']
        #Normal order of the options
        results = run_command("gppkg -q --info %s" % self.alpha_spec.get_filename())
        assert len(results.split('\n')) > 1
        results = self.remove_timestamp(results)
        assert results == expected_info_result

        #Reverse order of the options
        results = run_command("gppkg --info -q %s" % self.alpha_spec.get_filename())
        assert len(results.split('\n')) > 1
        results = self.remove_timestamp(results)
        assert results == expected_info_result

    def test03_query_list(self):
        expected_list_result = []
        with closing(tarfile.open(self.alpha_spec.get_filename(), 'r:gz')) as tarinfo:
            expected_list_result = tarinfo.getnames()

        results = run_command("gppkg -q --list %s" % self.alpha_spec.get_filename())
        assert len(results.split('\n')) > 1
        results = results.split('\n')[self.start_output:self.end_output]
        assert results == expected_list_result
        
        results = run_command("gppkg --list -q %s" % self.alpha_spec.get_filename())
        assert len(results.split('\n')) > 1
        results = results.split('\n')[self.start_output:self.end_output] 
        assert results == expected_list_result

class MiscTestCase(GppkgTestCase):
    """Covers all test cases which do not fall under any specific category"""
    @unittest.expectedFailure
    def test00_MPP_14671(self):
        """
        This test ensures that gppkg does not use the package name
        to discern information about the package. 
        """
        gppkg_file = self.build(self.alpha_spec, self.A_spec)

        dummy_file = "dummy-x.y.z-abcd-efgh.gppkg"
        shutil.copy(gppkg_file, dummy_file)
        self.extra_clean.add(dummy_file)

        self.install(dummy_file)
        assert os.path.exists(os.path.join(GPHOME, 'share', 'packages', 'archive', dummy_file))

        self.check_rpm_install(self.A_spec.get_package_name())

        assert self.check_install(gppkg_file)

        results = run_command("gppkg -q --all")
        assert ''.join(gppkg_file.split('-')[:1]) in results

@requiresSpecialGphome()
class SymLinkTestCase(GppkgTestCase):
    """Tests to cover MPP-15429."""
    def setUp(self):
        super(SymLinkTestCase, self).setUp()
        self.gp_home = GPHOME
        self.gp_home_symlink = os.path.join(os.path.dirname(self.gp_home), "greenplum-db")

        self.segment_host_list = get_host_list()[1]
        self.standby = get_host_list()[0]
        assert len(self.segment_host_list) > 0 
     
        link_command = "cd $GPHOME/..; ln -s $GPHOME greenplum-db"
        self.symlink_operation(link_command)
 
    def tearDown(self):
        unlink_command = "cd $GPHOME/..; rm -rf greenplum-db"
        self.symlink_operation(unlink_command)
 
    def symlink_operation(self, command):
        #If we are using symlinks, then sourcing the 
        #greenplum_path.sh will set gphome to <symlink_path>/.
        #Otherwise, the gphome will not contain a /.
        if not self.gp_home.endswith('/.'):
            for host in self.segment_host_list:
                run_remote_command(command, host)
            if self.standby is not None:
                run_remote_command(command, self.standby)
            run_command(command)

    def test00_gppkg_dereferenced_symlink_path(self):
        """     
        This test case makes use of the dereferenced symlink path in order 
        to ensure that the rpm does not mess up the symlink path.
        If the GPHOME is a symlink, and we try to uninstall an rpm against
        an rpm database which uses the symlinked GPHOME, rpm tries to remove
        the symlink directory and all its contents. The system becomes unusable
        after this. This problem does not occur when we use a normal path 
        instead of a symlink while specifying the rpm database.  
        """
        gppkg_file = self.build(self.alpha_spec, self.A_spec)
        self.install(gppkg_file)
        self.remove(gppkg_file)

    def test01_gppkg_with_symlink_path(self):
        """
        This test installs a gppkg and then tries to uninstall 
        the gppkg against a symlink.
        """
        os.environ['GPHOME'] = self.gp_home_symlink
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)
        self.remove(gppkg_file)
        os.environ['GPHOME'] = self.gp_home
    
    def test02_rpm_with_symlink_path(self):
        """
        This test tries to install an rpm and then tries to uninstall 
        the rpm against a symlink.
        """
        os.environ['GPHOME'] = self.gp_home_symlink

        try:
            with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
                tf.extract(self.A_spec.get_filename())
            self.extra_clean.add(self.A_spec.get_filename())

            symlink_rpm_database = os.path.join(self.gp_home_symlink, "share/packages/database")
            self.install_rpm(self.A_spec.get_filename(), symlink_rpm_database, self.gp_home_symlink)
            self.uninstall_rpm(self.A_spec.get_filename())
            assert not os.path.islink(self.gp_home_symlink)

        finally:
            os.environ['GPHOME'] = self.gp_home
 
class SimpleNegativeTestCase(GppkgTestCase):
    @unittest.expectedFailure
    def test00_wrong_os(self):
        os = "abcde"
        A_spec = self.A_spec 
        alpha_spec = GppkgSpec("alpha", "1.0", GPDB_VERSION, os)
        gppkg_file = self.build(alpha_spec, A_spec) 

        with self.assertRaisesRegexp(ExecutionError , "%s os required. %s os found" % (os, OS)):
            self.install(gppkg_file)
       
    def test01_wrong_arch(self):
        arch = "abcde"
        A_spec = self.A_spec 
        alpha_spec = GppkgSpec("alpha", "1.0", GPDB_VERSION, OS, arch)
        gppkg_file = self.build(alpha_spec, A_spec) 

        with self.assertRaisesRegexp(ExecutionError, "%s Arch required. %s Arch found" % (arch, ARCH)):
            self.install(gppkg_file)

    def test02_wrong_gpdbversion(self):
        gpdb_version = "4.6"
        A_spec = self.A_spec 
        alpha_spec = GppkgSpec("alpha", "1.0", gpdb_version)
        gppkg_file = self.build(alpha_spec, A_spec)

        with self.assertRaisesRegexp(ExecutionError, "requires Greenplum Database version %s" % gpdb_version):
            self.install(gppkg_file)

    def test03_install_twice(self):
        gppkg_file = self.build(self.alpha_spec, self.A_spec)

        self.install(self.alpha_spec.get_filename())
 
        with self.assertRaisesRegexp(ExecutionError, "%s is already installed" % gppkg_file):
            self.install(gppkg_file)

    @unittest.expectedFailure
    def test04_update_gppkg_lower(self):
        """
        This test tries to update a gppkg which has a lower version, 
        but the main comprising rpm is of higher version than the one
        already installed on the system.
        """
        #Use gppkg from previous test
        self.install(self.alpha_spec.get_filname())

        #Use gppkg which has a lower version, but the rpm is > the one installed
        update_rpm_spec = RPMSpec("A", "1", "2")
        update_gppkg_spec = GppkgSpec("alpha", "0.1")
        update_gppkg_file = self.build(update_gppkg_spec, update_rpm_spec)
       
        with self.assertRaisesRegexp(ExecutionError, "Newer version of %s already installed" % update_gppkg_spec.get_package_name()):
            self.update(update_gppkg_file)
        #Check that the original package is still installed and not updated
        assert self.check_install(self.alpha_spec.get_filename())

    def test05_update_rpm_lower(self):
        """
        This test tries to install a gppkg which has a higher version,
        but the main comprising rpm is of lower version than the one
        already installed on the system.
        """
        #Use gppkg from previous test
        self.install(self.alpha_spec.get_filename())

        #Use gppkg with a lower RPM version but gppkg version is > the one installed
        update_rpm_spec = RPMSpec("A", "1", "0")
        update_gppkg_spec = GppkgSpec("alpha", "1.1") 
        update_gppkg_file = self.build(update_gppkg_spec, update_rpm_spec)
        
        with self.assertRaisesRegexp(ExecutionError, self.A_spec.get_filename()):
            self.update(update_gppkg_file)
        #Check that the original package is still installed and not updated
        assert self.check_install(self.alpha_spec.get_filename())

    def test06_uninstall_twice(self):
        #Uses the gppkg from previous test
        self.install(self.alpha_spec.get_filename())

        #Uninstall gppkg 
        self.remove(self.alpha_spec.get_filename())

        with self.assertRaisesRegexp(ExecutionError, "%s has not been installed" % self.alpha_spec.get_package_name()):
            self.remove(self.alpha_spec.get_filename())

class SingleDependenciesTestCase(GppkgTestCase):
    """Covers install/update/remove tests of gppkgs which have a single dependency"""
    def setUp(self):
        super(SingleDependenciesTestCase, self).cleanup()
        self.A_spec = RPMSpec("A", "1", "1", ["B = 1-1"])
        self.B_spec = RPMSpec("B", "1", "1")
        self.alpha_spec = GppkgSpec("alpha", "1.0")

    def test00_build(self):
        self.build(self.alpha_spec, self.A_spec, [self.B_spec])
       
    def test01_install(self):
        gppkg_file = self.build(self.alpha_spec, self.A_spec, [self.B_spec])
        self.install(gppkg_file)

    def test02_update_gppkg_higher(self):
        """
        This test tries to update a gppkg with a higher version 
        and the main and dependent rpms with a higher version.
        """
        #Use gppkg from previous test
        self.install(self.alpha_spec.get_filename())
        
        #New gppkg with higher gppkg, main and deps version
        update_main_rpm_spec = RPMSpec("A", "1", "2", ["B = 1-2"])
        update_dep_rpm_spec = RPMSpec("B", "1", "2")
        update_gppkg_spec = GppkgSpec("alpha", "1.1")
        update_gppkg_file = self.build(update_gppkg_spec, update_main_rpm_spec, [update_dep_rpm_spec]) 
        
        self.update(update_gppkg_file) 
   
    @unittest.expectedFailure 
    def test03_update_gppkg_lower(self):
        """
        This test tries to update a gppkg with a lower version 
        and the main and dependent rpms with a lower version.
        """
        #Use the gppkg from previous test
        update_main_rpm_spec = RPMSpec("A", "1", "2", ["B = 1-2"])
        update_dep_rpm_spec = RPMSpec("B", "1", "2")
        update_gppkg_spec = GppkgSpec("alpha", "1.1")
        self.install(update_gppkg_spec.get_filename())
    
        #Original gppkg with a lower gppkg, main and deps version
        with self.assertRaisesRegexp(ExecutionError, "A newer version of %s is already installed" % self.alpha_spec.get_filename()):
            self.update(self.alpha_spec.get_filename())

    def test04_uninstall(self):
        self.install(self.alpha_spec.get_filename())
        self.remove(self.alpha_spec.get_filename())

class SharedDependenciesTestCase(GppkgTestCase):
    """Covers install/update/remove of gppkgs which have a shared dependency"""
    def setUp(self):
        self.cleanup()

        self.alpha_spec = GppkgSpec("alpha", "1.0")
        self.A_spec = RPMSpec("A", "1", "1", ["B = 1-1", "shared >= 1-1"])
        self.B_spec = RPMSpec("B", "1", "1") 

        self.beta_spec = GppkgSpec("beta", "1.0")
        self.C_spec = RPMSpec("C", "1", "1", ["D = 1-1", "shared >= 1-1"])
        self.D_spec = RPMSpec("D", "1", "1")

        self.shared_dep_spec = RPMSpec("shared", "1", "1") 

    def tearDown(self):
        self.cleanup()
 
    def test00_build(self):
        self.build(self.alpha_spec, self.A_spec, [self.B_spec, self.shared_dep_spec])
        self.build(self.beta_spec, self.C_spec, [self.D_spec, self.shared_dep_spec])

    def test01_install(self):
        self.install(self.alpha_spec.get_filename())
        self.install(self.beta_spec.get_filename())
    
    def test02_uninstall(self):
        self.install(self.alpha_spec.get_filename())
        self.install(self.beta_spec.get_filename())
    
        self.remove(self.beta_spec.get_filename())
    
        self.check_rpm_install(self.shared_dep_spec.get_package_name())

    def test03_update(self):
        """
        Covers the case of update of gppkg with a shared dependency
        """
        self.install(self.alpha_spec.get_filename())
        self.install(self.beta_spec.get_filename()) 

        #package to be updated
        gamma_spec = GppkgSpec("alpha", "1.1")
        rpm_spec = RPMSpec("E", "1", "1", ["B = 1-1", "shared >= 2-1"])
        dep_spec = self.B_spec
        shared_dep_spec = RPMSpec("shared", "2", "1")
        self.build(gamma_spec, rpm_spec, [dep_spec, shared_dep_spec])
        
        self.update(gamma_spec.get_filename())

        self.check_rpm_uninstall(self.shared_dep_spec.get_package_name())
        self.check_rpm_install(shared_dep_spec.get_package_name())

    def test04_install_dependencies_incompatible(self):
        """
        Install gppkg A with dependencies B, C
        Install gppkg D with dependencies B, E 
        where version(D.B) < version(A.B)
        """
        shared_dep_spec = RPMSpec("shared", "2", "1")
        F_spec = RPMSpec("F", "1", "1")
        E_spec = RPMSpec("E", "1", "1", ["F = 1-1", "shared = 2-1"])
        gamma_spec = GppkgSpec("gamma", "1.0")
        self.build(gamma_spec, E_spec, [F_spec, shared_dep_spec]) 

        self.install(gamma_spec.get_filename())

        #The error generated by gppkg when we try to install a package with a lower version
        #when a higher version is already installed will be of the form
        #gppkg:fips1:subraa4-[INFO]:-Starting gppkg with args: --install test-1.0-Linux-x86_64.gppkg
        #gppkg:fips1:subraa4-[INFO]:-Installing package test-1.0-Linux-x86_64.gppkg
        #gppkg:fips1:subraa4-[INFO]:-Validating rpm installation cmdStr='rpm --test -i /data/abhijits/greenplum/.tmp/dep-1-1.x86_64.rpm /data/abhijits/greenplum/.tmp/shared-1-1.x86_64.rpm /data/abhijits/greenplum/.tmp/test-1-1.x86_64.rpm --dbpath /data/abhijits/greenplum/share/packages/database --prefix /data/abhijits/greenplum'
        #gppkg:fips1:subraa4-[INFO]:-Exception = 'shared-2-1.x86_64.rpm'
        #gppkg:fips1:subraa4-[INFO]:-Exception = 'shared-2-1.x86_64.rpm'
        #gppkg:fips1:subraa4-[CRITICAL]:-gppkg failed. (Reason=''shared-2-1.x86_64.rpm'') exiting...

        #rpm --test command blows up and that is the main reason for the error.
        #rpm --test -i shared-2-1.x86_64.rpm gives the following error when a newer version is already installed,
        #   package shared-1-1.x86_64 is already installed
        with self.assertRaises(ExecutionError):
            self.install(self.alpha_spec.get_filename())

        self.check_rpm_uninstall(self.shared_dep_spec.get_package_name())
        
        self.check_rpm_install(shared_dep_spec.get_package_name())

    def test05_update_dependencies_incompatible(self):
        """
        Installs gppkg A with dependencies B, C
        Update gppkg A' with dependencies B, C
        where version(A'.B) < version(A.B) and version(A') > version(A)
        """
        update_gppkg_spec = GppkgSpec("alpha", "2.0")
        B_spec = self.B_spec
        A_spec = RPMSpec("A", "1", "1", ["B = 1-1", "shared = 2-1"])
        shared_dep_spec = RPMSpec("shared", "2", "1")
        self.build(update_gppkg_spec, A_spec, [B_spec, shared_dep_spec])

        self.install(update_gppkg_spec.get_filename())
       
        with self.assertRaises(ExecutionError): 
            self.update(self.alpha_spec.get_filename()) 

    def test06_MPP_15788(self):
        """
        This test covers the case where we want to update a package
        but are unable to do so due to the restrictions imposed by 
        another version of the dependency already installed on the system.
        """

        A_spec = RPMSpec("A", "1", "1", ["B = 1-1", "shared = 1-1"])
        C_spec = RPMSpec("C", "1", "1", ["D = 1-1", "shared = 1-1"])
        
        self.build(self.alpha_spec, A_spec, [self.B_spec, self.shared_dep_spec])
        self.build(self.beta_spec, C_spec, [self.D_spec, self.shared_dep_spec])
        
        self.install(self.alpha_spec.get_filename()) 
        self.install(self.beta_spec.get_filename()) 

        #package to be updated
        update_gppkg_spec = GppkgSpec("alpha", "1.1")
        A_spec = RPMSpec("A", "2", "1", ["B = 2-1", "shared = 2-1"])
        B_spec = RPMSpec("B", "2", "1")
        shared_dep_spec = RPMSpec("shared", "2", "1")
        self.build(update_gppkg_spec, A_spec, [B_spec, shared_dep_spec]) 
       
        with self.assertRaises(ExecutionError): 
            self.update(update_gppkg_spec.get_filename())

    @unittest.expectedFailure
    def test07_MPP_15789(self):
        """
        This test covers the case of obsolete dependencies.
        When we update a gppkg and the new gppkg has different
        dependencies from the original gppkg, the dependencies 
        of the original gppkg will get left behind. When we 
        remove the gppkg, only the newer dependencies will be removed
        and the old ones will still remain on the system. 
        """

        update_gppkg_spec = GppkgSpec("alpha", "1.1")
        A_spec = RPMSpec("A", "2", "1", ["B = 1-1"])
        B_spec = self.B_spec
        
        self.build(update_gppkg_spec, A_spec, [B_spec])
        
        self.install(self.alpha_spec.get_filename())
       
        self.update(update_gppkg_spec.get_filename())

        self.remove(update_gppkg_spec.get_filename()) 

        results = run_command("rpm -qa --dbpath %s" % RPM_DATABASE)
        assert results == ""
        
class MuckWithInternalsTestCase(GppkgTestCase):
    """
        Each test method in this suite should clean up
        whatever it "mucks". Otherwise it will leave the 
        system in an inconsistent state and the generic
        cleanup method will not be able to cleanup 
        the packages. 
    """
    def test00_delete_package_from_archive_and_install(self):
        """
        Delete a package from the archive on the master
        and try to install the gppkg again.
        """
        gppkg_file = self.build(self.alpha_spec, self.A_spec)
        self.install(gppkg_file) 

        #Remove the package from archive
        os.remove(os.path.join(ARCHIVE_PATH, gppkg_file))
       
        self.install(gppkg_file)

    @unittest.expectedFailure
    def test01_delete_package_from_archive_and_uninstall(self):
        """
        Delete a package from the archive on the master 
        and try to uninstall the gppkg.  
        Known issue: MPP-15737
        """
        #Building off of last test case's state
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)

        #Remove the package from archive
        os.remove(os.path.join(ARCHIVE_PATH, gppkg_file))

        try:
            self.remove(self.alpha_spec.get_filename())
        except ExecutionError, e:
            shutil.copy(gppkg_file, os.path.join(ARCHIVE_PATH, gppkg_file))
            self.fail("Execution Error %s" % e)

    def test02_delete_rpm_and_install(self):
        """
        Delete the main comprising rpm from the master
        and try to install the gppkg again.
        """
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)
    
        #Uninstall the RPM
        self.uninstall_rpm(self.A_spec.get_filename()) 
        self.install(gppkg_file) 

    def test03_delete_rpm_and_uninstall(self):
        """
        Delete the main comprising rpm from the master
        and try to install the gppkg again.
        """
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)
    
        #Uninstall the RPM
        self.uninstall_rpm(self.A_spec.get_filename())
        self.remove(gppkg_file) 

    def test04_install_rpm_and_install(self):
        """
        Install the main comprising rpm on the master
        and try to install a gppkg.
        """
        with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
            tf.extract(self.A_spec.get_filename())
          
        self.install_rpm(self.A_spec.get_filename()) 
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
      
        self.install(gppkg_file)

    @unittest.expectedFailure
    def test05_install_rpm_and_uninstall(self):
        """
        Install the main comprising rpm on the master and 
        try to uninstall a gppkg.
        """
        with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
            tf.extract(self.rpm_spec.get_filename())
          
        self.install_rpm(self.rpm_spec.get_filename())
        gppkg_file = self.alpha_spec.get_filename()
      
        try: 
            self.remove(gppkg_file)
        except ExecutionError, e:
            run_command("rpm -e %s --dbpath %s" % (self.A_spec.get_package_name(), RPM_DATABASE))
            os.remove(self.A_spec.get_filename())
            self.fail("ExecutionError %s" % e)

    @unittest.expectedFailure
    def test06_delete_package_from_archive_on_segment_and_install(self):
        """
        Delete a package from the archive on the segment 
        and try to install the gppkg again.
        """
        gppkg_file = self.alpha_spec.get_filename()
        archive_file = os.path.join(ARCHIVE_PATH, gppkg_file)
        self.install(gppkg_file)

        #Remove package from archive
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0
        host = segment_host_list[0]
        RemoveRemoteFile(archive_file ,host).run()

        try:
            self.install(gppkg_file)
        except ExecutionError, e:
            Scp(name = "copy gppkg to segment",
                srcFile = gppkg_file, 
                dstFile = archive_file, 
                srcHost = None,
                dstHost = host).run(validateAfter = True)
            self.fail("ExecutionError %s" % e)
           
    @unittest.expectedFailure 
    def test07_delete_package_from_archive_on_segment_and_uninstall(self):
        """
        Delete a package from the archive on the segment
        and try to uninstall the gppkg.
        """
        gppkg_file = self.alpha_spec.get_filename()
        archive_file = os.path.join(ARCHIVE_PATH, gppkg_file)
        self.install(gppkg_file)

        #Remove package from archive
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0
        host = segment_host_list[0]
        RemoveRemoteFile(archive_file ,host).run()

        try:
            self.remove(gppkg_file)
        except ExecutionError, e:
            GpScp(source_path = gppkg_file,
                  target_path = archive_file,
                  host_list = segment_host_list).run()
            self.fail("ExecutionError %s" % str(e))

    @unittest.expectedFailure
    def test08_uninstall_rpm_on_segments_and_install(self):
        """
        Try to install a gppkg again, when rpm has not been 
        installed on one of the segments.
        """
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)
   
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0
        host = segment_host_list[0]
 
        self.uninstall_rpm_remotely(self.a_spec.get_filename(), host)

        try:
            self.install(gppkg_file) 
        except ExecutionError, e:
            #Install the rpm 
            with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
                tf.extract(self.rpm_spec.get_filename())
            Scp(name = "copy rpm to segment", 
                srcFile = self.rpm_spec.get_filename(), 
                dstFile = self.rpm_spec.get_filename(), 
                srcHost = None,
                dstHost = host).run(validateAfter = True)
            self.install_rom_remotely(self.A_spec.get_filename())
            os.remove(self.A_spec.get_filename())
            RemoveRemoteFile(self.A_spec.get_filename(), host).run()
            self.fail("ExecutionError %s" % e)

    def test09_uninstall_rpm_on_segments_and_uninstall(self):
        """
        Try to uninstall a gppkg when the rpm has already been 
        uninstalled on one of the segments.
        """
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)
   
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0 
        host = segment_host_list[0]
 
        self.uninstall_rpm_remotely(self.A_spec.get_filename(), host)
        self.remove(gppkg_file) 

    def test10_install_rpm_on_segments_and_install(self):
        """
        Try to install a gppkg when an rpm has already been 
        installed on one of the segments.
        """
        gppkg_file = self.alpha_spec.get_filename()
 
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0
        host = segment_host_list[0]
 
        #Install the rpm 
        with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
            tf.extract(self.A_spec.get_filename())
        Scp(name = "copy rpm to segment", 
            srcFile = self.A_spec.get_filename(), 
            dstFile = self.A_spec.get_filename(), 
            srcHost = None, 
            dstHost = host).run(validateAfter = True)

        self.install_rpm_remotely(self.A_spec.get_filename(), host)
        self.install(gppkg_file) 

    @unittest.expectedFailure
    def test11_install_rpm_on_segments_and_uninstall(self):
        """
        Try to uninstall a gppkg, when the gppkg has not been installed,
        but the main comprising rpm has been installed on one of the segments.
        The desired result is that the rpm should be uninstalled.
        """
        gppkg_file = self.alpha_spec.get_filename()
        
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0
        host = segment_host_list[0]

        #Install the rpm
        with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
            tf.extract(self.A_spec.get_filename())
        Scp(name = "copy rpm to segment",
              srcFile = self.A_spec.get_filename(),
              dstFile = self.A_spec.get_filename(), 
              srcHost = None,
              dstHost = host).run(validateAfter = True)

        self.install_rpm_remotely(self.A_spec.get_filename(), host)

        try:
            self.remove(gppkg_file)
        except ExecutionError, e:
            self.uninstall_rpm_remotely(self.A_spec.get_filename(), host)
            os.remove(self.A_spec.get_filename())
            RemoveRemoteFile(self.A_spec.get_filename(), host).run()
            self.fail("ExecutionError %s" % e)

@skipIfNoStandby()
class MuckWithInternalsOnStandby(GppkgTestCase):            
    @unittest.expectedFailure
    def test00_delete_package_from_archive_on_standby_and_install(self):
        gppkg_file = self.build(self.alpha_spec, self.A_spec)
        archive_file = os.path.join(ARCHIVE_PATH, gppkg_file)
        self.install(gppkg_file)

        #Remove package from standby
        standby = get_host_list()[0] 
        RemoveRemoteFile(os.path.join(ARCHIVE_PATH, gppkg_file), standby).run()

        try:
            self.install(gppkg_file)
        except ExecutionError, e:
            Scp(name = "copy gppkg to standby",
                srcFile = gppkg_file,
                dstFile = archive_file,
                srcHost = None,
                dstHost = standby).run(validateAfter = True)
            self.fail("ExecutionError %s" % e)

    @unittest.expectedFailure
    def test01_delete_package_from_archive_on_standby_and_uninstall(self):
        '''
            Known issue: MPP-15737
        '''
        gppkg_file = self.alpha_spec.get_filename()
        archive_file = os.path.join(ARCHIVE_PATH, gppkg_file)
        self.install(gppkg_file)

        #Remove package from standby
        standby = get_host_list()[0] 
        RemoveRemoteFile(os.path.join(ARCHIVE_PATH, gppkg_file), standby).run()

        try:
            self.remove(gppkg_file)
        except ExecutionError, e:
            GpScp(source_path = gppkg_file,
                  target_path = archive_file,
                  host_list = get_host_list()[1]).run()
            Scp(name = "copy gppkg to standby",
                srcFile = gppkg_file,
                dstFile = archive_file, 
                srcHost = None, 
                dstHost = standby).run(validateAfter = True)
            self.fail("ExecutionError %s" % e)

    @unittest.expectedFailure
    def test02_uninstall_rpm_on_standby_and_install(self):
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)
   
        standby = get_host_list()[0]
 
        self.uninstall_rpm_remotely(self.A_spec.get_filename(), standby)
        try:
            self.install(gppkg_file) 
        except ExecutionError, e:
            #Install the rpm 
            with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
                tf.extract(self.A_spec.get_filename())
            Scp(name = "copy rpm to standby", 
                source_path = self.A_spec.get_filename(), 
                target_path = self.A_spec.get_filename(), 
                srcHost = None,
                dstHost = standby).run(validateAfter = True)
            self.install_rpm_remotely(self.A_spec.get_filename(), standby)
            os.remove(self.A_spec.get_filename())
            RemoveRemoteFile(self.A_spec.get_filename(), standby).run()
            self.fail("ExecutionError %s" % e)

    def test03_uninstall_rpm_on_standby_and_uninstall(self):
        #Use gppkg from previous test
        gppkg_file = self.alpha_spec.get_filename()
        self.install(gppkg_file)
   
        standby = get_host_list()[0]
 
        self.uninstall_rpm_remotely(self.A_spec.get_filename(), standby)
        self.remove(gppkg_file) 

    def test04_install_rpm_on_standby_and_install(self):
        gppkg_file = self.alpha_spec.get_filename()
 
        standby = get_host_list()[0]
 
        #Install the rpm 
        with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
            tf.extract(self.A_spec.get_filename())
        Scp(name = "copy the rpm to standby",
            srcFile = self.A_spec.get_filename(),
            dstFile = self.A_spec.get_filename(), 
            srcHost = None, 
            dstHost = standby).run(validateAfter = True)

        self.install_rpm_remotely(self.A_spec.get_filename(), standby)
        self.install(gppkg_file) 

    @unittest.expectedFailure
    def test05_install_rpm_on_standby_and_uninstall(self):
        gppkg_file = self.alpha_spec.get_filename()
        
        standby = get_host_list()[0]

        #Install the rpm
        with closing(tarfile.open(self.alpha_spec.get_filename())) as tf:
            tf.extract(self.A_spec.get_filename())
        Scp(name = "copy rpm to standby",
            srcFile = self.A_spec.get_filename(),
            dstFile = self.A_spec.get_filename(), 
            srcHost = None,
            dstHost = standby).run(validateAfter = True)

        self.install_rpm_remotely(self.A_spec.get_filename())

        try:
            self.remove(gppkg_file)
        except ExecutionError, e:
            self.uninstall_rpm_remotely(self.A_spec.get_filename(), standby)
            os.remove(self.A_spec.get_filename())
            RemoveRemoteFile(self.A_spec.get_filename(), standby).run()
            self.fail("ExecutionError %s" % e)

class CleanGppkgTestCase(GppkgTestCase):
    def setUp(self):
        super(CleanGppkgTestCase, self).setUp()
        self.clean_command = "gppkg --clean"

        standby, segment_host_list = get_host_list()
        cluster_host_list = [standby] + [host for host in segment_host_list]        

    def test00_build(self):
        self.build(self.alpha_spec, self.A_spec)

    def test01_install(self):
        self.install(self.alpha_spec.get_filename())

    def test02_no_packages_on_cluster(self):
        """
        This test covers the case when we run clean 
        with no packages installed on the cluster
        """
        run_command(self.clean_command) 
        standby, segment_host_list = get_host_list()
        cluster_host_list = [standby] + [host for host in segment_host_list]        

        for host in cluster_host_list:
            if host is not None:
                self.check_remote_rpm_uninstall(self.A_spec.get_package_name(), host)
                assert not CheckRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), host).run()

    def test03_no_package_on_master(self):
        """
        This test covers the case when there is no
        package installed on the master, but some
        package is installed on the rest of the cluster
        """
        self.install(self.alpha_spec.get_filename())
        
        os.remove(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()))
     
        self.uninstall_rpm(self.A_spec.get_filename())

        run_command(self.clean_command) 
        standby, segment_host_list = get_host_list()
        cluster_host_list = [standby] + [host for host in segment_host_list]
       
        for host in cluster_host_list:
            if host is not None:
                self.check_remote_rpm_uninstall(self.A_spec.get_package_name(), host)
                assert not CheckRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), host).run()

    def test04_no_package_on_segment(self):
        """
        This test covers the case when there is no
        package installed on one of the segment, but
        it is installed on the master and everywhere else 
        """
        self.install(self.alpha_spec.get_filename())
       
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0
        host = segment_host_list[0]       
 
        RemoveRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), host).run()

        self.uninstall_rpm_remotely(self.A_spec.get_filename(), host)

        run_command(self.clean_command)

        self.check_remote_rpm_install(self.A_spec.get_package_name(), host)
        assert CheckRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), host).run()

    @unittest.expectedFailure
    def test05_no_rpm_on_master(self):
        """
        This test covers the case when the rpm has been 
        uninstalled on the master and the gppkg is in the archive, 
        and the rpms/gppkgs are intstalled on the segments.   
        JIRA - MPP-15968
        """
        self.install(self.alpha_spec.get_filename())
        
        self.uninstall_rpm(self.A_spec.get_filename())
        run_command(self.clean_command)

        self.check_rpm_install(self.A_spec.get_package_name())
        assert os.path.exists(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()))

    @unittest.expectedFailure
    def test06_no_rpm_on_segment(self):
        """
        This test covers the case when the gppkg has been installed
        on the cluster and the rpm has not been installed properly on
        one of the segments. The gppkg however exists in the archive on
        the segment. 
        """
        self.install(self.alpha_spec.get_filename())
        
        segment_host_list = get_host_list()[1]
        assert len(segment_host_list) > 0
        host = segment_host_list[0]

        self.uninstall_rpm_remotely(self.A_spec.get_filename(), host)
        run_command(self.clean_command)

        self.check_remote_rpm_install(self.A_spec.get_package_name(), host)
        assert CheckRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), host).run()

    @skipIfNoStandby()
    def test07_no_package_on_standby(self):
        """
        This test covers the case when there is no package
        installed on the standby, but the package is installed
        across the other hosts in the cluster.
        JIRA - MPP-15969
        """ 
        self.install(self.alpha_spec.get_filename())

        standby = get_host_list()[0]
        
        RemoveRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), standby).run()
        self.uninstall_rpm_remotely(self.A_spec.get_filename(), standby)
        run_command(self.clean_command)
    
        self.check_remote_rpm_install(self.A_spec.get_package_name(), standby)
        assert CheckRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), standby).run()
        
    @skipIfNoStandby()
    @unittest.expectedFailure
    def test08_no_rpm_on_standby(self):
        """
        Covers the case when there is no rpm installed on the standby
        but the gppkg is present in the archive and the package has 
        been installed across other hosts in the cluster.
        JIRA - MPP-15968, MPP-15969
        """ 
        self.install(self.alpha_spec.get_filename())
        
        standby = get_host_list()[0]
    
        self.uninstall_rpm_remotely(self.A_spec.get_filename(), standby)
        run_command(self.clean_command)

        self.check_remote_rpm_install(self.A_spec.get_package_name(), standby)
        assert CheckRemoteFile(os.path.join(ARCHIVE_PATH, self.alpha_spec.get_filename()), standby).run()

if __name__ == "__main__":
    unittest.main()
