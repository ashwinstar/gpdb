%define maindir     %(echo `cd ../../../../..;pwd`)
%define _topdir     %{maindir}/cdb-pg/src/pl/plperl/package 
%define name        perl 
%define release     8 
%define version     5.8 
%define buildroot   %{_topdir}/perl-%{version}.%{release} 
%define buildarch   %(uname -p) 
%define __os_install_post   %{nil}

BuildRoot:      %{buildroot}
Summary:        Perl 
License:        Artistic License        
Name:           %{name}
Version:        %{version}
Release:        %{release} 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       perl-%{version}-%{release}.%{buildarch}.rpm 

%description
The Perl package provides Perl for PL/Perl. 

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/temp/share/packages/perl
cp -r %{maindir}/ext/%{arch}/perl-%{version}.%{release}/*   %{buildroot}/temp/share/packages/perl

%files
/temp
