%define maindir     %(echo `cd ../../../../..;pwd`)
%define plperldir   %{maindir}/cdb-pg/src/pl/plperl
%define _topdir     %{plperldir}/package
%define buildroot   %{_topdir}/install 
%define buildarch   %(uname -p) 
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
Summary:        PL/Perl for Greenplum Database 
License:        Artistic License        
Name:           plperl
Version:        %{plperl_ver}
Release:        %{plperl_rel}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
#/bin/sh is added to trick rpm to believe that the dependency /bin/sh is satisfied
Provides:       plperl-%{plperl_ver}-%{plperl_rel}.%{buildarch}.rpm /bin/sh
Requires:       perl-%{perl_ver}-%{perl_rel}.%{buildarch}.rpm

%description
The PL/Perl package provides Procedural language implementation of Perl for Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/temp
make -C %{_topdir} install prefix=install/temp

%files
/temp
