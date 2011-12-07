SHELL=/bin/bash

BLD_TOP=$(shell echo `pwd`/../../)
include $(BLD_TOP)/Makefile.global
include $(BLD_TOP)/Makefile.thirdparty

GPMONWS_DIR=../src/webui/webapi
GPMONUI_DIR=../src/webui/flexGUI
GPMONDOCS_DIR=../docs/release
LIGHTY_DIR=$(BLD_TOP)/ext/$(BLD_ARCH)/lighttpd-1.4.28/
GPPERFMON_VERSION=$(shell perl -pe 's, ,-,g' ../../VERSION)
CONSOLEDOCS_DIR=../../docs/release/html/perfmon_console_help

osx104_x86_PACKAGE_ARCH=OSX-i386
osx105_x86_PACKAGE_ARCH=OSX-i386
rhel4_x86_32_PACKAGE_ARCH=RHEL4-i386
rhel4_x86_64_PACKAGE_ARCH=RHEL4-x86_64
rhel5_x86_32_PACKAGE_ARCH=RHEL5-i386
rhel5_x86_64_PACKAGE_ARCH=RHEL5-x86_64
sol10_x86_64_PACKAGE_ARCH=SOL-x86_64
sol10_sparc_64_PACKAGE_ARCH=SOL10_sparc_64
suse10_x86_64_PACKAGE_ARCH=SuSE10-x86_64
suse11_x86_64_PACKAGE_ARCH=SuSE10-x86-64
suse11.1_x86_64_PACKAGE_ARCH=SuSE10-x86_64
PACKAGE_ARCH=$($(BLD_ARCH)_PACKAGE_ARCH)

#---------------------------------------------------------------------
# Set Version
#---------------------------------------------------------------------

SET_VERSION_SCRIPTS = \
	gpperfmon/installer/.tmp/bin/gpperfmon.py \
	gpperfmon/installer/.tmp/www/gpmonws.py \
	$(NULL)

set_gpmon_scripts_version :
	@for file in $(SET_VERSION_SCRIPTS); do \
	    pushd ../.. ; \
	    if [ -f $${file} ]; then \
	        chmod a+w $${file} ; \
	        perl $(BLD_TOP)/putversion $${file} ; \
	        chmod a-w $${file} ; \
	    fi ; \
	    popd ; \
	done

#---------------------------------------------------------------------
#
#---------------------------------------------------------------------

all:
	@if [ "$(PACKAGE_ARCH)" = "" ]; then \
		echo Unknown platform; \
		exit 1; \
	fi;
	@if [ -d .tmp ]; then \
		rm -rf .tmp; \
	fi;
	@if [ -a greenplum-perfmon-web.tar.gz ]; then \
		rm -f greenplum-perfmon-web.tar.gz; \
	fi;
	mkdir .tmp
	mkdir .tmp/bin
	mkdir .tmp/etc
	mkdir .tmp/lib
	mkdir .tmp/lib/python
	mkdir .tmp/www
	mkdir .tmp/www/static
	mkdir .tmp/www/static/docs
	mkdir .tmp/instances
	cp ../src/gpperfmon ../src/gpperfmon.py .tmp/bin
	cp -rp $(BLD_TOP)/gpMgmt/bin/gppylib .tmp/lib/python
	cp -rp $(BLD_TOP)/gpMgmt/bin/ext/* .tmp/lib/python
	mkdir -p .tmp/ext/python
	cp -rp $(BLD_PYTHON_PERFMON)/* .tmp/ext/python
	cp -R $(GPMONWS_DIR)/* .tmp/www
	cp -R $(GPMONUI_DIR)/bin-release/* .tmp/www/static
	cp -R $(LIGHTY_DIR)/* .tmp
	cp -R $(CONSOLEDOCS_DIR)/* .tmp/www/static/docs
	cp $(BLD_TOP)/ext/$(BLD_ARCH)/bin/openssl .tmp/bin
	cp $(BLD_TOP)/ext/$(BLD_ARCH)/ssl/openssl.cnf .tmp/etc	
	@if [ "$(PACKAGE_ARCH)" = "OSX-i386" ] ; then \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libpcre.0.0.1.dylib .tmp/lib; \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libcrypto.0.9.8.dylib .tmp/lib; \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libssl.0.9.8.dylib .tmp/lib; \
		cd .tmp/lib; \
		ln -s libpcre.0.0.1.dylib libpcre.0.dylib; \
		ln -s libssl.0.9.8.dylib libssl.dylib; \
		ln -s libcrypto.0.9.8 libcrypto.dylib; \
		cd ../..; \
	else \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libpcre.so.0.0.1 .tmp/lib; \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libcrypto.so.0.9.8 .tmp/lib; \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libssl.so.0.9.8 .tmp/lib; \
		cd .tmp/lib; \
		ln -s libpcre.so.0.0.1 libpcre.so.0; \
		ln -s libpcre.so.0.0.1 libpcre.so; \
		ln -s libssl.so.0.9.8 libssl.so; \
		ln -s libcrypto.so.0.9.8 libcrypto.so; \
		cd ../..; \
	fi ;
	$(MAKE) set_gpmon_scripts_version
	./generate-gpperfmon-path.sh > .tmp/gpperfmon_path.sh
	(cd .tmp && tar cf - * | gzip -c > ../greenplum-perfmon-web.tar.gz)
	./create-gpperfmon-installer.sh
	mv greenplum-perfmon-web.bin greenplum-perfmon-web-$(GPPERFMON_VERSION)-$(PACKAGE_ARCH).bin
	rm -rf .tmp
	zip -q greenplum-perfmon-web-$(GPPERFMON_VERSION)-$(PACKAGE_ARCH).zip greenplum-perfmon-web-$(GPPERFMON_VERSION)-$(PACKAGE_ARCH).bin

clean:
	rm -rf .tmp
	rm -f greenplum-perfmon*.tar.gz greenplum-perfmon*.bin greenplum-perfmon*.zip
