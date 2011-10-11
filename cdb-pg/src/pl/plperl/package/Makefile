# TODO: AK: plperl isn't actually being built against the perl packaged here.
# I argue this should be addressed one directory higher. The same issue exists for pljava. 
MACHINE_ARCH=$(shell uname -p)
OS=$(shell uname -s)

PERL_VER=5.12.4
PERL_REL=1
PERL_DIR=$(RELENG_TOOLS)/perl-$(PERL_VER)
PERL_RPM=perl-$(PERL_VER)-$(PERL_REL).$(MACHINE_ARCH).rpm

PLPERL_VER=1.0
PLPERL_REL=1
PLPERL_RPM=plperl-$(PLPERL_VER)-$(PLPERL_REL).$(MACHINE_ARCH).rpm
PLPERL_GPPKG=plperl-$(PLPERL_VER)-$(OS)-$(MACHINE_ARCH).gppkg

all: $(PLPERL_GPPKG)

BLD_TOP=../../../../..
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec --define "perl_ver $(PERL_VER)" --define "plperl_ver $(PLPERL_VER)" \
										--define "perl_rel $(PERL_REL)" --define "plperl_rel $(PLPERL_REL)" \
										--define "perl_dir $(PERL_DIR)"
	mv RPMS/$(MACHINE_ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS

$(PLPERL_GPPKG):	$(PERL_DIR) $(PERL_RPM) $(PLPERL_RPM)
	sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml
	sed -i".bk" "s/#os/$(OS)/g" gppkg/gppkg_spec.yml
	rm -f gppkg/*.bk
	rm -rf gppkg/deps
	mkdir gppkg/deps
	cp $(PERL_RPM) gppkg/deps
	cp $(PLPERL_RPM) gppkg/
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg

clean:
	rm -rf RPMS SRPMS SOURCES BUILD
	rm -rf gppkg/deps
	rm -rf perl-$(PLPERL_VER)
	rm -f $(PERL_GPPKG) $(PERL_RPM) $(PLPERL_RPM)

.PHONY: clean
