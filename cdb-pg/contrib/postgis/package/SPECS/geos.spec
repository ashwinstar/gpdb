%define maindir     %(echo `cd ../../../..;pwd`)
%define _topdir     %{maindir}/cdb-pg/contrib/postgis/package
%define name        geos 
%define release     1
%define version     3.2.2
%define buildroot   %{_topdir}/geos 
%define buildarch   %(uname -p)
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
Summary:        Geos library 
License:        LGPL        
Name:           %{name}
Version:        %{version}
Release:        %{release} 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       geos-%{version}-%{release}.%{buildarch}.rpm 

%description
The Geos module provides geometric library which is used by PostGIS.

%prep
rm -rf %{buildroot}
mkdir -p %{buildroot}/temp/lib

%install
cp -rf %{geos_dir}/lib/libgeos* %{buildroot}/temp/lib/

%files
/temp
