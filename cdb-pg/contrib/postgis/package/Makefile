#Build gppkg

all: gppkg

BLD_TOP=../../../../
include $(BLD_TOP)/Makefile.global     # for BLD_ARCH
GEOS_DIR=$(BLD_TOP)/ext/$(BLD_ARCH)/geos-3.2.2
GEOS_CONFIG=$(GEOS_DIR)/bin/geos-config
PROJ_DIR=$(BLD_TOP)/ext/$(BLD_ARCH)/proj-4.7.0
MACHINE_ARCH=$(shell uname -p)
OS=$(shell uname -s)
GPMGMT=../../../../gpMgmt

gppkg:
ifeq ( , $(wildcard $(BLD_TOP)/ext/$(BLD_ARCH)/proj-4.7.0))
	echo 'proj-4.7.0 could not be found'
	echo 'exiting postgis build'
	exit 1	
endif
ifeq ( , $(wildcard $(BLD_TOP)/ext/$(BLD_ARCH)/geos-3.2.2))
	echo 'geos-3.2.2 could not be found'
	echo 'exiting postgis build'
	exit 1
endif
	sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml
	sed -i".bk" "s/#os/$(OS)/g" gppkg/gppkg_spec.yml
	rm -f gppkg/*.bk
	rm -rf RPMS SOURCES BUILD SRPMS
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/postgis.spec --define "geosversion 3.2-2"   --define "projversion 4.7-0" 
	rpmbuild -bb SPECS/geos.spec --define "arch $(BLD_ARCH)" 
	rpmbuild -bb SPECS/proj.spec --define "arch $(BLD_ARCH)" 
	cp RPMS/$(MACHINE_ARCH)/postgis* gppkg/ 
	rm -rf gppkg/deps 
	mkdir gppkg/deps 
	cp RPMS/$(MACHINE_ARCH)/geos*   gppkg/deps 
	cp RPMS/$(MACHINE_ARCH)/proj*   gppkg/deps 
	$(GPMGMT)/bin/gppkg -b gppkg 

clean:
	rm -rf RPMS SOURCES BUILD SRPMS
	rm -rf gppkg/deps
	rm -rf geos
	rm -rf proj
	rm gppkg/*.rpm

.PHONY: gppkg clean
