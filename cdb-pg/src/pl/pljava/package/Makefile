#Build gppkg

all: gppkg

MACHINE_ARCH=$(shell uname -p)
OS=$(shell uname -s)
JAVA_VERSION=1.6.0_26
BLD_TOP=../../../../..
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/Makefile.global

gppkg:
ifneq (, $(wildcard $(BLD_TOP)/ext/$(BLD_ARCH)/jre$(JAVA_VERSION)))
		sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml 
		sed -i".bk" "s/#os/$(OS)/g" gppkg/gppkg_spec.yml
		rm -f gppkg/*.bk
		rm -rf SRPMS SOURCES RPMS BUILD
		mkdir SRPMS SOURCES RPMS BUILD
		rpmbuild -bb SPECS/java.spec --define "arch $(BLD_ARCH)"
		rpmbuild -bb SPECS/pljava.spec --define "javaver 1.6.0" --define "javarel 26" --define "libvar LD_LIBRARY_PATH"
		cp RPMS/$(MACHINE_ARCH)/pljava*	gppkg/ 
		rm -rf gppkg/deps 
		mkdir gppkg/deps 
		cp RPMS/$(MACHINE_ARCH)/java* gppkg/deps 
		cp RPMS/$(MACHINE_ARCH)/pljava* gppkg/ 
		$(GPMGMT)/bin/gppkg -b gppkg 
else 
		echo "JAVA could not be found for $(BLD_ARCH)"
		echo "Aborting JAVA build"
endif

clean:
	rm -rf RPMS SRPMS SOURCES BUILD
	rm -rf gppkg/deps
	rm -rf java$(JAVA_VERSION)
	rm  gppkg/*.rpm

.PHONY: gppkg clean
