#!/usr/bin/env python

import os
import shutil
import unittest2 as unittest
import tempfile
import platform
import getpass
import tarfile

from contextlib import closing
from gppylib.commands import gp
from gppylib.commands.base import Command, ExecutionError
from gppylib.operations import Operation
from gppylib.operations.unix import CheckFile

OS = platform.system()
ARCH = platform.machine()
GPHOME = gp.get_gphome()
ARCHIVE_PATH = os.path.join(GPHOME, 'share/packages/archive')
RPM_DATABASE = os.path.join(GPHOME, 'share/packages/database') 
GPPKG_EXTENSION = ".gppkg"
SCRATCH_SPACE = os.path.join(tempfile.gettempdir(), getpass.getuser())
GPDB_VERSION = "4.2"

def clean_up():
    results = run_command("gppkg -q --all")
    gppkgs = results.split('\n')[1:]   #The first line is 'Starting gppkg with args', which we want to ignore. 

    for gppkg in gppkgs:
        run_command("gppkg --remove " + gppkg)

def build_and_check(test_instance, gppkg_spec, rpm_spec, rpm_specs = []):
    gppkg_file = BuildGppkg(gppkg_spec, rpm_spec, rpm_specs).run()
    test_instance.assertTrue(check_gppkg_build(gppkg_file, gppkg_spec))
    return gppkg_file

def check_gppkg_build(gppkg_file, gppkg_spec):
    return (gppkg_file == (gppkg_spec.get_filename()))

def check_gppkg_install(gppkg_filename, is_installed = True):
    cmd = "gppkg -q %s" % gppkg_filename
    results = run_command(cmd)
    results = results[1:] 
    test_str = ''.join(gppkg_filename.split('-')[:1]) 
    test_str += " is installed" if is_installed else " is not installed"
    return True if test_str in results else False

def remove_timestamp(result):
    result_without_timestamp = []
    results = result.split('\n')[1:]
    
    for res in results:
        res = res.split(':-')[1].strip()
        res = ''.join(res.split(' '))
        result_without_timestamp.append(res)

    return result_without_timestamp 

def run_command(cmd_str):
    cmd = Command("Local Command", cmd_str)
    cmd.run(validateAfter = True)
    results = cmd.get_results()
    
    if results.rc != 0:
        return results.stderr.strip()
    else:
        return results.stdout.strip()

class RPMSpec:
    def __init__(self, name, version, release, depends = []):
        self.name = name
        self.version = version
        self.release = release
        self.depends = depends

    def get_package_name(self):
        return self.name + '-' + self.version + '-' + self.release 

    def get_filename(self):
        return self.get_package_name() + '.' + ARCH + ".rpm"

    def __str__(self):
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
Provides:       temp_test = '''+ self.version +''', /bin/sh
BuildRoot:      %{_topdir}/BUILD '''

        if self.depends != []: 
            '''Requires:       ''' + ' '.join(self.depends)
        
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
    def __init__(self, name, version, gpdbversion = GPDB_VERSION, os = OS, arch = ARCH):
        self.name = name 
        self.version = version
        self.gpdbversion = gpdbversion
        self.os = os
        self.arch = arch

    def get_package_name(self):
        return self.name + '-' + self.version       

    def get_filename(self):
        return self.get_package_name() + '-' + self.os + '-' + self.arch + GPPKG_EXTENSION

    def __str__(self): 
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

class SimpleGppkgTestCases(unittest.TestCase):
    def setUp(self):
        clean_up()

        self.rpm_spec = RPMSpec("test", "1", "1")
        self.gppkg_spec = GppkgSpec("test", "1.0")

    def tearDown(self):
        clean_up()

    def test00_simple_build(self):
        build_and_check(self, self.gppkg_spec, self.rpm_spec)

    def test01_simple_install(self):
        gppkg_file = self.gppkg_spec.get_filename() 
        run_command("gppkg --install " + gppkg_file)

        #Check RPM database
        results = run_command("rpm -q %s --dbpath %s" % (self.rpm_spec.get_package_name(), RPM_DATABASE))
        self.assertEquals(results, self.rpm_spec.get_package_name())

        #Check Gppkg database
        results = run_command("gppkg -q %s" % gppkg_file)
        self.assertTrue(check_gppkg_install(gppkg_file))

        #Check the ARCHIVE_PATH
        self.assertTrue(CheckFile(os.path.join(ARCHIVE_PATH, gppkg_file)).run())

    def test02_simple_update(self):
        gppkg_file = self.gppkg_spec.get_filename() 
        run_command("gppkg --install %s" % gppkg_file)
        
        update_rpm_spec = RPMSpec("test", "1", "2")
        update_gppkg_spec = GppkgSpec("test", "1.1")
        update_gppkg_file = build_and_check(self, update_gppkg_spec, update_rpm_spec)
   
        run_command("gppkg --update %s" % update_gppkg_file) 

        #Check for the packages
        results = run_command("rpm -q %s --dbpath %s" % (update_rpm_spec.get_package_name(), RPM_DATABASE))
        self.assertEquals(results, update_rpm_spec.get_package_name())

        results = run_command("gppkg -q %s" % update_gppkg_file)
        self.assertTrue(check_gppkg_install(update_gppkg_file))

    def test03_simple_uninstall(self):
        gppkg_file = self.gppkg_spec.get_filename()

        run_command("gppkg --install %s" % gppkg_file)
        run_command("gppkg --remove %s" % self.gppkg_spec.get_package_name())

        results = run_command("gppkg -q --all")
        results = results.split('\n')[1:]
        
        self.assertEquals(results, [])     
       
    def test04_help(self):
        help_options = ["--help", "-h", "-?"] 

        for opt in help_options:
            results = run_command("gppkg " + opt)
            self.assertNotEquals(results, "")

    def test05_version(self):
        results = run_command("gppkg --version")
        self.assertNotEquals(results, "")

class QueryTestCases(unittest.TestCase):
    def setUp(self):
        self.rpm_spec1 = RPMSpec("test", "1", "1")    
        self.gppkg_spec1 = GppkgSpec("test", "1.0")
 
        self.rpm_spec2 = RPMSpec("test1", "1", "1")
        self.gppkg_spec2 = GppkgSpec("test1", "1.0")

    def test00_install(self):
        gppkg_file1 = build_and_check(self, self.gppkg_spec1, self.rpm_spec1)
        gppkg_file2 = build_and_check(self, self.gppkg_spec2, self.rpm_spec2)
 
        run_command("gppkg --install %s" % gppkg_file1)
        results = run_command("gppkg -q %s" % gppkg_file1)
        self.assertTrue(check_gppkg_install(gppkg_file1))
        
        run_command("gppkg --install %s" % gppkg_file2)
        results = run_command("gppkg -q %s" % gppkg_file2)
        self.assertTrue(check_gppkg_install(gppkg_file2))
  
    def test01_query_all(self):
        results = run_command("gppkg -q --all")
        self.assertEquals(results.split('\n')[1:], [self.gppkg_spec1.get_package_name(), self.gppkg_spec2.get_package_name()])

        results = run_command("gppkg --all -q")
        self.assertEquals(results.split('\n')[1:], [self.gppkg_spec1.get_package_name(), self.gppkg_spec2.get_package_name()])

        self.assertRaises(ExecutionError, run_command, "gppkg -qall")

    def test02_query_info(self):
        expected_info_result = ['Nametest', 'Version1.0', 'Architecturex86_64', 'OSLinux', 'GPDBVersionmainbuilddev', 'DescriptionTemporaryTestPackage']
        #Normal order of the options
        results = run_command("gppkg -q --info %s" % self.gppkg_spec1.get_filename())
        self.assertTrue(len(results.split('\n')) > 1)
        results = remove_timestamp(results)
        self.assertEquals(results, expected_info_result)

        #Reverse order of the options
        results = run_command("gppkg --info -q %s" % self.gppkg_spec1.get_filename())
        self.assertTrue(len(results.split('\n')) > 1)
        results = remove_timestamp(results)
        self.assertEquals(results, expected_info_result)

    def test03_query_list(self):
        expected_list_result = []
        with closing(tarfile.open(self.gppkg_spec1.get_filename(), 'r:gz')) as tarinfo:
            expected_list_result = tarinfo.getnames()

        results = run_command("gppkg -q --list %s" % self.gppkg_spec1.get_filename())
        self.assertTrue(len(results.split('\n')) > 1)
        results = results.split('\n')[1:]
        self.assertEquals(results, expected_list_result)
        
        results = run_command("gppkg --list -q %s" % self.gppkg_spec1.get_filename())
        self.assertTrue(len(results.split('\n')) > 1)
        results = results.split('\n')[1:] 
        self.assertEquals(results, expected_list_result)

class MiscTestCases(unittest.TestCase):
    def setUp(self):
        clean_up()

    def tearDown(self):
        clean_up()

    @unittest.expectedFailure
    def test00_MPP_14671(self):
        rpm_spec = RPMSpec("test", "1", "1")
        gppkg_spec = GppkgSpec("test", "1.0")
        gppkg_file = build_and_check(self, gppkg_spec, rpm_spec)

        dummy_file = "dummy-x.y.z-abcd-efgh.gppkg"
        shutil.copy(gppkg_file, dummy_file)
        run_command("gppkg --install %s" % dummy_file)

        self.assertTrue(os.path.exists(os.path.join(GPHOME, 'share', 'packages', 'archive', dummy_file)))

        results = run_command("rpm -q %s --dbpath %s" % (rpm_spec.get_package_name(), RPM_DATABASE))
        self.assertEquals(results, rpm_spec.get_package_name())

        results = run_command("gppkg -q %s" % gppkg_spec.get_filename())
        self.assertTrue(check_gppkg_install(gppkg_file))

        results = run_command("gppkg -q --all")
        self.assertTrue(''.join(gppkg_file.split('-')[:1]) in results)

class SimpleNegativeTestCases(unittest.TestCase):
    def setUp(self):
        clean_up()
        self.rpm_spec = RPMSpec("test", "1", "1")
        self.gppkg_spec = GppkgSpec("test", "1.0")

    def tearDown(self):
        clean_up()

    @unittest.expectedFailure
    def test00_wrong_os(self):
        os = "abcde"
        rpm_spec = self.rpm_spec 
        gppkg_spec = GppkgSpec("test", "1.0", GPDB_VERSION, os)
        gppkg_file = build_and_check(self, gppkg_spec, rpm_spec) 

        with self.assertRaisesRegexp(ExecutionError , "%s os required. %s os found" % (os, OS)):
            run_command("gppkg --install %s" % gppkg_file)
       
    def test01_wrong_arch(self):
        arch = "abcde"
        rpm_spec = self.rpm_spec 
        gppkg_spec = GppkgSpec("test", "1.0", GPDB_VERSION, OS, arch)
        gppkg_file = build_and_check(self, gppkg_spec, rpm_spec) 

        with self.assertRaisesRegexp(ExecutionError, "%s Arch required. %s Arch found" % (arch, ARCH)):
            run_command("gppkg --install %s" % gppkg_file)

    def test02_wrong_gpdbversion(self):
        gpdb_version = "4.6"
        rpm_spec = self.rpm_spec 
        gppkg_spec = GppkgSpec("test", "1.0", gpdb_version)
        gppkg_file = build_and_check(self, gppkg_spec, rpm_spec)

        with self.assertRaisesRegexp(ExecutionError, "requires Greenplum Database version %s" % gpdb_version):
            run_command("gppkg --install %s" % gppkg_file)

    def test03_install_twice(self):
        gppkg_file = build_and_check(self, self.gppkg_spec, self.rpm_spec)

        run_command("gppkg --install %s" % gppkg_file)
        self.assertTrue(check_gppkg_install(gppkg_file))
       
        with self.assertRaisesRegexp(ExecutionError, "%s is already installed" % gppkg_file):
            run_command("gppkg --install %s" % gppkg_file)
         
    @unittest.expectedFailure
    def test04_update_gppkg_lower(self):
        #Use gppkg from previous test
        run_command("gppkg --install %s" % self.gppkg_spec.get_filename())
        self.assertTrue(check_gppkg_install(self.gppkg_spec.get_filename()))

        #Use gppkg which has a lower version, but the rpm is > the one installed
        update_rpm_spec = RPMSpec("test", "1", "2")
        update_gppkg_spec = GppkgSpec("test1", "0.1")
        update_gppkg_file = build_and_check(self, update_gppkg_spec, update_rpm_spec)
       
        with self.assertRaisesRegexp(ExecutionError, "Newer version of %s already installed" % update_gppkg_spec.get_package_name()):
            run_command("gppkg --update %s" % update_gppkg_file) 
        #Check that the original package is still installed and not updated
        self.assertTrue(check_gppkg_install(self.gppkg_spec.get_filename()))

    def test05_update_rpm_lower(self):
        #Use gppkg from previous test
        run_command("gppkg --install %s" % self.gppkg_spec.get_filename())
        self.assertTrue(check_gppkg_install(self.gppkg_spec.get_filename()))

        #Use gppkg with a lower RPM version but gppkg version is > the one installed
        update_rpm_spec = RPMSpec("test", "1", "0")
        update_gppkg_spec = GppkgSpec("test1", "1.1") 
        update_gppkg_file = build_and_check(self, update_gppkg_spec, update_rpm_spec)
        
        with self.assertRaisesRegexp(ExecutionError, self.rpm_spec.get_filename()):
            run_command("gppkg --update %s" % update_gppkg_file)
        #Check that the original package is still installed and not updated
        self.assertTrue(check_gppkg_install(self.gppkg_spec.get_filename()))
        
    def test06_uninstall_twice(self):
        #Uses the gppkg from previous test
        run_command("gppkg --install %s" % self.gppkg_spec.get_filename())
        self.assertTrue(check_gppkg_install(self.gppkg_spec.get_filename()))

        #Uninstall gppkg 
        run_command("gppkg --remove %s" % self.gppkg_spec.get_package_name())
        self.assertTrue(check_gppkg_install(self.gppkg_spec.get_filename(), is_installed = False))
    
        with self.assertRaisesRegexp(ExecutionError, "%s has not been installed" % self.gppkg_spec.get_package_name()):
            run_command("gppkg --remove %s" % self.gppkg_spec.get_package_name())
        
if __name__ == "__main__":
    unittest.main()
