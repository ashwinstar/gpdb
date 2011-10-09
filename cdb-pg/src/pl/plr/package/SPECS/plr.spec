%define maindir     %(echo `cd ../../../../..;pwd`)
%define plrdir      %{maindir}/cdb-pg/src/pl/plr
%define _topdir     %{plrdir}/package
%define buildroot   %{_topdir}/install 
%define buildarch   %(uname -p)
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
Summary:        PL/R for greenplum database 
License:        GPLv2       
Name:           plr
Version:        %{plr_ver}
Release:        %{plr_rel}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
#/bin/sh is added to trick rpm to believe that the dependency /bin/sh is satisfied 
Provides:       plr-%{plr_ver}-%{plr_rel}.%{buildarch}.rpm /bin/sh
Requires:       R-%{r_ver}-%{r_rel}.%{buildarch}.rpm

%description
The PL/R modules provides Procedural language implementation of R for Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}
make -C %{plrdir} install prefix=package/install/temp R_HOME=%{r_dir}/lib64/R

%files
/temp
