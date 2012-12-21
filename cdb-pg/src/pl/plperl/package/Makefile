all: gppkg

BLD_TOP=../../../../../
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk
include $(BLD_TOP)/Makefile.global

OS=$(word 1,$(subst _, ,$(BLD_ARCH)))
ARCH=$(shell uname -p)

PERL_VER=5.12.4
PERL_REL=1
PERL_DIR=$(RELENG_TOOLS)/perl-$(PERL_VER)
PERL_RPM=perl-$(PERL_VER)-$(PERL_REL).$(ARCH).rpm

PLPERL_VER=1.0
PLPERL_REL=1
PLPERL_RPM=plperl-$(PLPERL_VER)-$(PLPERL_REL).$(ARCH).rpm
PLPERL_GPPKG=plperl-$(PLPERL_VER)-$(OS)-$(ARCH).gppkg

gppkg: $(PLPERL_GPPKG)

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec --define "perl_ver $(PERL_VER)" --define "plperl_ver $(PLPERL_VER)" \
										--define "perl_rel $(PERL_REL)" --define "plperl_rel $(PLPERL_REL)" \
										--define "perl_dir $(PERL_DIR)"
	mv RPMS/$(ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS

gppkg/gppkg_spec.yml: gppkg/gppkg_spec.yml.bk
	cat $< | sed "s/#arch/$(ARCH)/g" | sed "s/#os/$(OS)/g" > $@

$(PLPERL_GPPKG): gppkg/gppkg_spec.yml $(PERL_DIR) $(PERL_RPM) $(PLPERL_RPM)
	rm -rf gppkg/deps
	mkdir gppkg/deps
	cp $(PERL_RPM) gppkg/deps
	cp $(PLPERL_RPM) gppkg/
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg

clean:
	rm -rf RPMS SRPMS SOURCES BUILD
	rm -rf gppkg/deps
	rm -rf install
	rm -rf perl-$(PLPERL_VER).$(PLPERL_REL)
	rm -f $(PERL_GPPKG) $(PERL_RPM) $(PLPERL_RPM)

.PHONY: gppkg clean
