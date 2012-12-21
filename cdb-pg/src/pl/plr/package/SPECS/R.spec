%define maindir     %(echo `cd ../../../../..;pwd`)
%define _topdir     %{maindir}/cdb-pg/src/pl/plr/package 
%define buildroot   %{_topdir}/R-%{r_ver}.%{r_rel} 
%define buildarch   %(uname -p) 
%define __os_install_post %{nil}

BuildRoot:      %{buildroot}
SUmmary:        R statistics language 
License:        GPLv2        
Name:           R
Version:        %{r_ver}
Release:        %{r_rel} 
Group:          Development/Tools
Prefix:         /temp
AutoReq:        no
AutoProv:       no
BuildArch:      %{buildarch} 
Provides:       R-%{r_ver}-%{r_rel}.%{buildarch}.rpm 

%description
The R module provides the R statistics language.

%prep
rm -rf %{buildroot}

%install
mkdir -p %{buildroot}/temp/ext/R-%{r_ver}
cp -r %{r_dir}/*  %{buildroot}/temp/ext/R-%{r_ver}

%post
echo "export R_HOME=\$GPHOME/ext/R-%{r_ver}/lib64/R" >> $GPHOME/greenplum_path.sh
echo "export LD_LIBRARY_PATH=\$GPHOME/ext/R-%{r_ver}/lib64/R/lib:\$LD_LIBRARY_PATH" >> $GPHOME/greenplum_path.sh

%postun
sed -i".bk" "s|export R_HOME=\$GPHOME/ext/R-%{r_ver}/lib64/R||g" $GPHOME/greenplum_path.sh
sed -i".bk" "s|export LD_LIBRARY_PATH=\$GPHOME/ext/R-%{r_ver}/lib64/R:\$LD_LIBRARY_PATH||g" $GPHOME/greenplum_path.sh
rm -rf $GPHOME/greenplum_path.sh.bk

%files
/temp
