%define maindir     %(echo `cd ../../../../..;pwd`)
%define plrdir      %{maindir}/cdb-pg/src/pl/plr
%define _topdir     %{plrdir}/package
%define name        plr
%define release     0
%define version     1
%define buildroot   %{_topdir}/install 
%define buildarch   %(uname -p)
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
Summary:        PL/R for greenplum database 
License:        GPLv2       
Name:           %{name}
Version:        %{version}
Release:        %{release}
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
#/bin/sh is added to trick rpm to believe that the dependency /bin/sh is satisfied 
Provides:       plr-%{version}-%{release}.%{buildarch}.rpm /bin/sh
Requires:       R-%{rver}-%{rrel}.%{buildarch}.rpm

%description
The PL/R modules provides Procedural language implementation of R for Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}
make -C %{plrdir} install prefix=package/install/temp R_HOME=%{maindir}/ext/%{arch}/R-%{rversion}/%{lib}/R
install %{maindir}/ext/%{arch}/R-%{rversion}/%{lib}/R/lib/lib* %{buildroot}/temp/lib

%post
echo "export R_HOME=$GPHOME/%{rpath}" >> $GPHOME/greenplum_path.sh
echo "export %{libvar}=$GPHOME/%{rpath}:\$%{libvar}" >> $GPHOME/greenplum_path.sh 

%postun
sed -i".bk" "s|export R_HOME=$GPHOME/%{rpath}||g" $GPHOME/greenplum_path.sh
sed -i".bk" "s|export %{libvar}=$GPHOME/%{rpath}:\$%{libvar}||g" $GPHOME/greenplum_path.sh
rm -rf $GPHOME/greenplum_path.sh.bk

%files
/temp
