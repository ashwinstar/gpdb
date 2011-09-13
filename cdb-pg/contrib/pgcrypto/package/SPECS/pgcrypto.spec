%define pgcrypto_dir %(echo `cd ..;pwd`)
%define _topdir     %(echo `pwd`) 
%define name        pgcrypto
%define release     0
%define version     1
%define buildroot   %{_topdir}/install
%define buildarch   %(uname -p)
%define __os_install_post    %{nil}

BuildRoot:      %{buildroot}
Summary:        Cryptographic functions for Greenplum Database 
License:        PostgreSQL License        
Name:           %{name}
Version:        %{version}
Release:        %{release}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       pgcrypto-%{version}-%{release}.%{buildarch}.rpm 

%description
The Pgcrypto package provides cryptographic package for the Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/
make -C %{pgcrypto_dir} install prefix=package/install/temp

%files
/temp
