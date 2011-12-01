#!/usr/bin/env python

import os
import shutil
import unittest
import tempfile
import platform

from gppylib.commands import gp
from gppylib.commands.base import Command
from gppylib.operations import Operation
from gppylib.operations.unix import CheckFile

OS = platform.system()
ARCH = platform.machine()
GPHOME = gp.get_gphome()
ARCHIVE_PATH = os.path.join(GPHOME, 'share/packages/archive')
RPM_DATABASE = os.path.join(GPHOME, 'share/packages/database') 
GPPKG_EXTENSION = ".gppkg"

class RPMSpec:
    def __init__(self, name, version, release, depends = []):
        self.name = name
        self.version = version
        self.release = release
        self.depends = depends

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
        scratch_space = tempfile.gettempdir()
        rpm_spec_file = str(self.spec)
        build_dir = os.path.join(scratch_space, "BUILD") 
        shutil.rmtree(build_dir, ignore_errors = True)
        os.mkdir(build_dir)

        rpms_dir = os.path.join(scratch_space, "RPMS")
        shutil.rmtree(rpms_dir, ignore_errors = True) 
        os.mkdir(rpms_dir)

        with tempfile.NamedTemporaryFile() as f:
            f.write(rpm_spec_file)
            f.flush()
            
            os.system("cd " + scratch_space + "; rpmbuild -bb " + f.name)

        shutil.move(os.path.join(scratch_space, "RPMS", ARCH, self.spec.name + '-' + self.spec.version + '-' + self.spec.release + '.' + ARCH + ".rpm"), os.getcwd())
        return self.spec.name + '-' + self.spec.version + '-' + self.spec.release + '.' + ARCH + ".rpm"

class GppkgSpec:
    def __init__(self, name, version, gpdbversion):
        self.name = name 
        self.version = version
        self.gpdbversion = gpdbversion
       
    def __str__(self): 
        gppkg_spec_file = '''
PkgName: ''' + self.name + '''
Version: ''' + self.version + '''
GPDBVersion: ''' + self.gpdbversion + '''
Description: Temporary Test Package
OS: ''' + OS + '''
Architecture: ''' + ARCH

        return gppkg_spec_file

class BuildGppkg(Operation):
    def __init__(self, gppkg_spec, main_rpm_spec, dependent_rpm_specs = []):
        self.gppkg_spec = gppkg_spec
        self.main_rpm_spec = main_rpm_spec
        self.dependent_rpm_specs = dependent_rpm_specs

    def execute(self):
        gppkg_spec_file = str(self.gppkg_spec)
        #create gppkg_dir
        gppkg_dir = os.path.join(tempfile.gettempdir(), "package")
        if not os.path.exists(gppkg_dir):
            os.mkdir(gppkg_dir)
            
        rpm_file = BuildRPM(self.main_rpm_spec).run()    
        shutil.move(rpm_file, gppkg_dir)

        if self.dependent_rpm_specs != []:
            deps_dir = os.path.join(gppkg_dir, "deps")
            os.mkdir(deps_dir)

            for spec in self.dependent_rpm_specs:
                rpm_file = BuildRPM(spec).run()
                shutil.move(rpm_file, deps_dir)

        #create gppkg
        with open(os.path.join(gppkg_dir, "gppkg_spec.yml"), "w") as f:
            f.write(gppkg_spec_file)

        Command("Build package", "gppkg --build " + gppkg_dir).run(validateAfter = True) 

        shutil.rmtree(gppkg_dir)
        return self.gppkg_spec.name + '-' + self.gppkg_spec.version + '-' + OS + '-' + ARCH + GPPKG_EXTENSION
        
class SimpleGppkgInstall(unittest.TestCase):
    def cleanUp(self):
        cmd = Command("List Packages", "gppkg -qall")
        cmd.run(validateAfter = True)
        
        gppkgs = cmd.get_results().stdout.strip().split('\n')[1:]   #The first line is 'Starting gppkg with args', which we want to ignore. 

        print 'Uninstalling gppkgs ...'
        for gppkg in gppkgs:
            Command("Remove Package", "gppkg --remove " + gppkg).run(validateAfter = True)

    def setUp(self):
        self.cleanUp()

        self.rpm_spec = RPMSpec("test", "1", "1")
        self.gppkg_spec = GppkgSpec("test", "1.0", "4.2")

    def tearDown(self):
        self.cleanUp()

    def test00_simple_build(self):
        gppkg_file = BuildGppkg(self.gppkg_spec, self.rpm_spec).run()
        self.assertTrue(gppkg_file == (self.gppkg_spec.name + '-' + self.gppkg_spec.version + '-' + OS + '-' + ARCH + GPPKG_EXTENSION)) 

    def test01_simple_install(self):
        gppkg_file = self.gppkg_spec.name + '-' + self.gppkg_spec.version + '-' + OS + '-' + ARCH + GPPKG_EXTENSION
        print 'Installing gppkg %s ...' % gppkg_file
        cmd = Command("Install gppkg", "gppkg --install " + gppkg_file).run(validateAfter = True)

        #Check RPM database
        cmd = Command("Query rpm database", "rpm -q %s --dbpath %s" % (self.rpm_spec.name + '-' + self.rpm_spec.version + '-' + self.rpm_spec.release, RPM_DATABASE))
        cmd.run(validateAfter = True)
        self.assertEquals(cmd.get_results().stdout.strip(), self.rpm_spec.name + '-' + self.rpm_spec.version + '-' + self.rpm_spec.release)

        #Check Gppkg database
        cmd = Command("Query gppkg database", "gppkg -q %s" % gppkg_file)
        cmd.run(validateAfter = True)
        self.assertEquals(''.join(cmd.get_results().stdout.strip().split('\n')[1:]), "%s is installed." % ''.join(gppkg_file.split('-')[:1]))

        #Check the ARCHIVE_PATH
        self.assertTrue(CheckFile(os.path.join(ARCHIVE_PATH, gppkg_file)).run())
        
if __name__ == "__main__":
    unittest.main()
