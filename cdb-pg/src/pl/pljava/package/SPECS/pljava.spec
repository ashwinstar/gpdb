%define maindir     %(echo `cd ../../../../..;pwd`)
%define pljavadir   %{maindir}/cdb-pg/src/pl/pljava
%define _topdir     %{pljavadir}/package
%define buildarch   %(uname -p) 
%define buildroot   %{_topdir}/BUILD
%define __os_install_post   %{nil}

BuildRoot:		%{buildroot}
Summary:        PL/Java for Greenplum Database 
License:        BSD        
Name:           pljava
Version:        %{pljava_ver}
Release:        %{pljava_rel}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
#/bin/sh is added to trick rpm to believe that the dependency /bin/sh is satisfied 
Provides:       pljava-%{pljava_ver}_%{pljava_rel}.%{buildarch}.rpm /bin/sh 
Requires:       jre-%{jre_ver}-%{jre_rel}.%{buildarch}.rpm

%description
The PL/Java package provides Procedural language implementation of Java for Greenplum Database. 

%install
mkdir -p %{buildroot}/temp
make -C %{pljavadir} install prefix=package/install/temp

%files
/temp
