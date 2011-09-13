#Build gppkg

all: gppkg

BLD_TOP=../../../../..
GPMGMT=$(BLD_TOP)/gpMgmt/bin
include $(BLD_TOP)/Makefile.global
MACHINE_ARCH=$(shell uname -p)
OS=$(shell uname -s)
PERL_VERSION=5.8.8

ifeq ($(OS), Darwin)
	LIBVAR=DYLD_LIBRARY_PATH
	PERLPATH=5.8.8/darwin-2level/CORE
else
	LIBVAR=LD_LIBRARY_PATH
	PERLPATH=5.8.8/x86_64-linux/CORE
endif

gppkg:
ifneq ( , $(wildcard $(BLD_TOP)/ext/$(BLD_ARCH)/perl-$(PERL_VERSION)))
	sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml
	sed -i".bk" "s/#os/$(OS)/g" gppkg/gppkg_spec.yml
	rm -f gppkg/*.bk
	rm -rf RPMS SOURCES SRPMS BUILD
	mkdir BUILD RPMS SRPMS SOURCES
	rpmbuild -bb SPECS/perl.spec --define "arch $(BLD_ARCH)"
	rpmbuild -bb SPECS/plperl.spec --define "arch $(BLD_ARCH)" --define "perlver 5.8" --define "perlrel 8" --define "libvar $(LIBVAR)" --define "perlpath $(PERLPATH)"
	rm -rf gppkg/deps
	mkdir gppkg/deps
	cp RPMS/$(MACHINE_ARCH)/perl* gppkg/deps
	cp RPMS/$(MACHINE_ARCH)/plperl* gppkg/
	$(GPMGMT)/gppkg -b gppkg
else 
	echo "Perl could not be found for $(BLD_ARCH)"
	echo "Aborting Perl build"
endif 

clean:
	rm -rf RPMS SRPMS SOURCES BUILD
	rm -rf gppkg/deps
	rm -rf perl-$(PERL_VERSION)
	rm gppkg/*.rpm

.PHONY: gppkg clean
