%define maindir     %(echo `cd ../../../..;pwd`)
%define _topdir     %{maindir}/cdb-pg/contrib/postgis/package 
%define name        proj 
%define release     0
%define version     4.7 
%define buildroot   %{_topdir}/proj 
%define buildarch   %(uname -p)
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
Summary:        Proj library 
License:        MIT License        
Name:           %{name}
Version:        %{version}
Release:        %{release} 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       proj-%{version}-%{release}.%{buildarch}.rpm 

%description
The Proj module provides cartographic projections library which is used by PostGIS.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/temp/share/packages
cp -rf %{maindir}/ext/%{arch}/proj-%{version}.%{release}   %{buildroot}/temp/share/packages 
rm -f %{buildroot}/temp/share/packages/checksums*

%files
/temp
