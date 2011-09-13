%define maindir     %(echo `cd ../../../../..;pwd`)
%define pljavadir   %{maindir}/cdb-pg/src/pl/pljava
%define _topdir     %{pljavadir}/package
%define name        pljava 
%define release     0 
%define version     1 
%define buildroot   %{_topdir}/install
%define buildarch   %(uname -p) 
%define __os_install_post   %{nil}

BuildRoot:      %{buildroot}
Summary:        PL/Java for Greenplum Database 
License:        BSD        
Name:           %{name}
Version:        %{version}
Release:        %{release} 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
#/bin/sh is added to trick rpm to believe that the dependency /bin/sh is satisfied 
Provides:       pljava-%{version}_%{release}.%{buildarch}.rpm /bin/sh 
Requires:       java-%{javaver}-%{javarel}.%{buildarch}.rpm

%description
The PL/Java package provides Procedural language implementation of Java for Greenplum Database. 

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/temp
make -C %{pljavadir} install prefix=package/install/temp

%post
echo "export JAVA_HOME=$GPHOME/share/packages/java" >> $GPHOME/greenplum_path.sh
echo "export PATH=$GPHOME/share/packages/java/bin:\$PATH" >> $GPHOME/greenplum_path.sh
echo "export %{libvar}=$GPHOME/share/packages/java/lib/amd64/server:\$%{libvar}" >> $GPHOME/greenplum_path.sh

%postun
sed -i".bk" "s|export JAVA_HOME=$GPHOME/share/packages/java||g" $GPHOME/greenplum_path.sh
sed -i".bk" "s|export PATH=$GPHOME/share/packages/java/bin:\$PATH||g" $GPHOME/greenplum_path.sh
sed -i".bk" "s|export %{libvar}=$GPHOME/share/packages/java/lib/amd64/server:\$%{libvar}||g" $GPHOME/greenplum_path.sh
rm -rf $GPHOME/greenplum_path.sh.bk

%files
/temp
