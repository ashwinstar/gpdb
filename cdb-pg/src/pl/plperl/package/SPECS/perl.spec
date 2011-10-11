%define maindir     %(echo `cd ../../../../..;pwd`)
%define _topdir     %{maindir}/cdb-pg/src/pl/plperl/package 
%define buildroot   %{_topdir}/perl-%{version}.%{release} 
%define buildarch   %(uname -p) 
%define __os_install_post   %{nil}

BuildRoot:      %{buildroot}
Summary:        Perl 
License:        Artistic License        
Name:           perl
Version:        %{perl_ver}
Release:        %{perl_rel} 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       perl-%{perl_ver}-%{perl_rel}.%{buildarch}.rpm 

%description
The Perl package provides Perl for PL/Perl. 

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/temp/ext/perl-%{perl_ver}
cp -r %{perl_dir}/perl-%{perl_ver}/*   %{buildroot}/temp/ext/perl-%{perl_ver}

%post
echo "export PATH=$GPHOME/ext/perl-%{perl_ver}/bin:\$PATH" >> $GPHOME/greenplum_path.sh
echo "export LD_LIBRARY_PATH=$GPHOME/ext/perl-%{perl_ver}/lib/5.12.4/x86_64-linux/CORE:\$LD_LIBRARY_PATH" >> $GPHOME/greenplum_path.sh

%postun
sed -i".bk" "s|export PATH=$GPHOME/ext/perl-%{perl_ver}/bin:\$PATH||g" $GPHOME/greenplum_path.sh
sed -i".bk" "s|export LD_LIBRARY_PATH=$GPHOME/ext/perl-%{perl_ver}/lib/5.12.4/x86_64-linux/CORE:\$LD_LIBRARY_PATH||g" $GPHOME/greenplum_path.sh
rm -rf $GPHOME/greenplum_path.sh.bk

%files
/temp
