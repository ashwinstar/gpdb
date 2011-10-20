%define postgis_dir %(echo `cd ..;pwd`)
%define _topdir     %{postgis_dir}/package 
%define name        postgis
%define release     1
%define version     1.0
%define buildroot   %{postgis_dir}/postgis/build
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
Requires:       geos = %{geos_ver}, proj = %{proj_ver}

%description
The PostGIS module provides geospatial extensions for Greenplum Database.

%prep
rm -rf %{buildroot}
mkdir -p %{buildroot}

%build
make -C %{postgis_dir}

%install
make -C %{postgis_dir} install prefix=build/temp 

%files
/temp
