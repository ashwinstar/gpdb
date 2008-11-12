SHELL=/bin/bash

BLD_TOP=$(shell echo `pwd`/../../)
include $(BLD_TOP)/Makefile.thirdparty

GPMON_DIR=../src/gpmon
GPMONWS_DIR=../src/webui/webapi
GPMONUI_DIR=../src/webui/flexGUI
GPMONDOCS_DIR=../docs/release
LIGHTY_DIR=$(BLD_TOP)/ext/$(BLD_ARCH)/lighttpd-1.4.19/
GPPERFMON_VERSION=$$(< $(BLD_TOP)/gpperfmon/VERSION)
PACKAGE_ARCH=

ifeq (osx104_x86, $(BLD_ARCH))
	PACKAGE_ARCH=OSX-i386
endif

ifeq (rhel4_x86_32, $(BLD_ARCH))
	PACKAGE_ARCH=RHEL4-i386
endif

ifeq (rhel4_x86_64, $(BLD_ARCH))
	PACKAGE_ARCH=RHEL4-x86_64
endif

ifeq (rhel5_x86_64, $(BLD_ARCH))
	PACKAGE_ARCH=RHEL5-x86_64
endif

ifeq (sol10_x86_64, $(BLD_ARCH))
	PACKAGE_ARCH=SOL-x86_64
endif

all:
	@if [ "$(PACKAGE_ARCH)" = "" ]; then \
		echo Unknown platform; \
		exit 1; \
	fi;
	@if [ -d .tmp ]; then \
		rm -rf .tmp; \
	fi;
	@if [ -a greenplum-perfmon.tar.gz ]; then \
		rm -f greenplum-perfmon.tar.gz; \
	fi;
	mkdir .tmp
	mkdir .tmp/bin
	mkdir .tmp/etc
	mkdir .tmp/lib
	mkdir .tmp/docs
	mkdir .tmp/www
	mkdir .tmp/www/static
	cp $(GPMON_DIR)/gpmmon $(GPMON_DIR)/gpsmon ../src/gpperfmon .tmp/bin
	cp $(GPMON_DIR)/gpmon_catqrynow.py .tmp/lib
	cp $(GPMON_DIR)/gpperfmon.sql .tmp
	cp -R $(GPMONWS_DIR)/* .tmp/www
	cp -R $(GPMONUI_DIR)/bin-release/* .tmp/www/static
	cp -R $(LIGHTY_DIR)/* .tmp
	cp $(GPMONDOCS_DIR)/GPMonGuide.pdf .tmp/docs
	cp $(GPMONDOCS_DIR)/GPMon_2000_README.pdf .tmp/docs
	@if [ "$(PACKAGE_ARCH)" = "OSX-i386" ] ; then \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libpcre.0.0.1.dylib .tmp/lib; \
		cd .tmp/lib; \
		ln -s libpcre.0.0.1.dylib libpcre.0.dylib; \
		cd ../..; \
	else \
		cp $(BLD_TOP)/ext/$(BLD_ARCH)/lib/libpcre.so.0.0.1 .tmp/lib; \
		cd .tmp/lib; \
		ln -s libpcre.so.0.0.1 libpcre.so.0; \
		ln -s libpcre.so.0.0.1 libpcre.so; \
		cd ../..; \
	fi ;
	./generate-gpperfmon-path.sh > .tmp/gpperfmon_path.sh
	cd .tmp && tar cvf ../greenplum-perfmon.tar *
	gzip greenplum-perfmon.tar
	./create-gpperfmon-installer.sh
	mv greenplum-perfmon.bin greenplum-perfmon-$(GPPERFMON_VERSION)-$(PACKAGE_ARCH).bin
	rm -rf .tmp

clean:
	rm -rf .tmp
	rm -f greenplum-perfmon.tar.gz greenplum-perfmon-$(GPPERFMON_VERSION)-$(PACKAGE_ARCH).bin
