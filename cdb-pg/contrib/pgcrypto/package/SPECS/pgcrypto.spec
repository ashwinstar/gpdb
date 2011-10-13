%define pgcrypto_dir %(echo `cd ..;pwd`)
%define _topdir     %(echo `pwd`) 
%define buildroot   %{_topdir}/install
%define buildarch   %(uname -p)
%define __os_install_post    %{nil}

BuildRoot:      %{buildroot}
Summary:        Cryptographic functions for Greenplum Database 
License:        PostgreSQL License        
Name:           pgcrypto
Version:        %{pgcrypto_ver}
Release:        %{pgcrypto_rel}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       pgcrypto-%{pgcrypto_ver}-%{pgcrypto_rel}.%{buildarch}.rpm 

%description
The Pgcrypto package provides cryptographic package for the Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/
make -C %{pgcrypto_dir} install prefix=package/install/temp

%files
/temp
