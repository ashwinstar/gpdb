%define gphdfs_dir %(echo `cd ..;pwd`)
%define _topdir     %(echo `pwd`) 
%define buildroot   %{_topdir}/install
%define buildarch   %(uname -p)
%define __os_install_post    %{nil}

BuildRoot:      %{buildroot}
Summary:        gphdfs for Greenplum Database
License:        EMC copyright        
Name:           gphdfs
Version:        %{gphdfs_ver}
Release:        %{gphdfs_rel}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       gphdfs-%{gphdfs_ver}-%{gphdfs_rel}.%{buildarch}.rpm 
Requires:       jre-%{jre_ver}-%{jre_rel}.%{buildarch}.rpm

%description
The gphdfs rpm provides gphdfs library for the Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/
make -C %{gphdfs_dir} install prefix=%{gphdfs_dir}/package/install/temp

%files
/temp
