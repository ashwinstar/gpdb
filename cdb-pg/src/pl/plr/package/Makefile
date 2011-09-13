#Build gppkg

all: gppkg

BLD_TOP=../../../../..
GPMGMT=../../../../../gpMgmt
MACHINE_ARCH=$(shell uname -p)
OS=$(shell uname -s)
RVERSION=2.13.0
include $(BLD_TOP)/Makefile.global

ifeq ($(OS), Darwin)
	LIBVAR=DYLD_LIBRARY_PATH
	RPATH=share/packages/R/lib/R
	LIB=lib
else 
	LIBVAR=LD_LIBRARY_PATH
	RPATH=share/packages/R/lib64/R
	LIB=lib64
endif

gppkg:
ifneq (, $(wildcard $(BLD_TOP)/ext/$(BLD_ARCH)/R-$(RVERSION)))
	sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml
	sed -i".bk" "s/#os/$(OS)/g"	gppkg/gppkg_spec.yml
	rm -f gppkg/*.bk
	rm -rf SOURCES SRPMS RPMS BUILD
	mkdir SOURCES SRPMS RPMS BUILD
	rpmbuild -bb SPECS/plr.spec --define "arch $(BLD_ARCH)" --define "rversion $(RVERSION)" --define "rver 2.13" --define "rrel 0" --define "libvar $(LIBVAR)" --define "rpath $(RPATH)" --define "lib $(LIB)"
	rpmbuild -bb SPECS/r.spec --define "arch $(BLD_ARCH)"
	cp RPMS/$(MACHINE_ARCH)/plr* gppkg/ 
	rm -rf gppkg/deps 
	mkdir gppkg/deps
	cp RPMS/$(MACHINE_ARCH)/R* gppkg/deps/ 
	$(GPMGMT)/bin/gppkg -b gppkg 
else 
   	echo "R-$(RVERSION) could not be found for $(BLD_ARCH)"
   	echo "Aborting R and PL/R build"
endif

clean:
	rm -rf RPMS SRPMS SOURCES BUILD
	rm -rf gppkg/deps
	rm -rf R-$(RVERSION)
	rm gppkg/*.rpm

.PHONY:gppkg clean
