%define maindir     %(echo `cd ../../../../..;pwd`)
%define plperldir   %{maindir}/cdb-pg/src/pl/plperl
%define _topdir     %{plperldir}/package
%define name        plperl 
%define release     0 
%define version     1 
%define buildroot   %{_topdir}/install 
%define buildarch   %(uname -p) 
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
Summary:        PL/Perl for Greenplum Database 
License:        Artistic License        
Name:           %{name}
Version:        %{version}
Release:        %{release} 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
#/bin/sh is added to trick rpm to believe that the dependency /bin/sh is satisfied
Provides:       plperl-%{version}-%{release}.%{buildarch}.rpm /bin/sh
Requires:       perl-%{perlver}-%{perlrel}.%{buildarch}.rpm

%description
The PL/Perl package provides Procedural language implementation of Perl for Greenplum Database.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/temp
make -C %{_topdir} install prefix=install/temp

%post
echo "export PATH=$GPHOME/share/packages/perl/bin:\$PATH" >> $GPHOME/greenplum_path.sh
echo "export %{libvar}=$GPHOME/share/packages/perl/lib/%{perlpath}:\$%{libvar}" >> $GPHOME/greenplum_path.sh

%postun
sed -i".bk" "s|export PATH=$GPHOME/share/packages/perl/bin:\$PATH||g" $GPHOME/greenplum_path.sh
sed -i".bk" "s|export %{libvar}=$GPHOME/share/packages/perl/lib/%{perlpath}:\$%{libvar}||g" $GPHOME/greenplum_path.sh
rm -rf $GPHOME/greenplum_path.sh.bk

%files
/temp
