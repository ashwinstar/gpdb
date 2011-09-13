%define maindir     %(echo `cd ../../../..;pwd`)
%define _topdir     %{maindir}/cdb-pg/contrib/postgis/package
%define name        geos 
%define release     2
%define version     3.2 
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

%install
mkdir -p %{buildroot}/temp/share/packages
cp -rf %{maindir}/ext/%{arch}/geos-%{version}.%{release}   %{buildroot}/temp/share/packages
rm -f %{buildroot}/temp/share/packages/geos-%{version}.%{release}/checksums*

%files
/temp
