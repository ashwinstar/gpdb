%define postgis_dir %(echo `cd ..;pwd`)
%define _topdir     %{postgis_dir}/package 
%define name        postgis
%define release     0
%define version     1
%define buildroot   %{postgis_dir}/postgis/install 
%define buildarch   %(uname -p) 
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
Summary:        Geospatial extensions for Greenplum Database 
License:        GPLv2        
Name:           %{name}
Version:        %{version}
Release:        %{release}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       postgis-%{version}-%{release}.%{buildarch}.rpm 
Requires:       geos-%{geosversion}.%{buildarch}.rpm proj-%{projversion}.%{buildarch}.rpm

%description
The PostGIS module provides geospatial extensions for Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}
make -C %{postgis_dir}
make -C %{postgis_dir}/postgis install prefix=install/temp
make -C %{postgis_dir} install prefix=install/temp 

%files
/temp
