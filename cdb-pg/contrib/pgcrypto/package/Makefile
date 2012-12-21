all: gppkg

BLD_TOP=../../../../
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk
include $(BLD_TOP)/Makefile.global

OS=$(word 1,$(subst _, ,$(BLD_ARCH)))
ARCH=$(shell uname -p)

PGCRYPTO_VER=1.0
PGCRYPTO_REL=1
PGCRYPTO_RPM=pgcrypto-$(PGCRYPTO_VER)-$(PGCRYPTO_REL).$(ARCH).rpm
PGCRYPTO_GPPKG=pgcrypto-$(PGCRYPTO_VER)-$(PGCRYPTO_REL)-$(OS)-$(ARCH).gppkg

gppkg: $(PGCRYPTO_GPPKG)

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec --define "pgcrypto_ver $(PGCRYPTO_VER)" --define "pgcrypto_rel $(PGCRYPTO_REL)"
	mv RPMS/$(ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS

gppkg/gppkg_spec.yml: gppkg/gppkg_spec.yml.bk
	cat $< | sed "s/#arch/$(ARCH)/g" | sed "s/#os/$(OS)/g" > $@

$(PGCRYPTO_GPPKG): gppkg/gppkg_spec.yml $(PGCRYPTO_RPM)  
	cp $(PGCRYPTO_RPM) gppkg/
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg/
						
clean:
	echo $(OS)
	rm -rf RPMS SOURCES SRPMS BUILD
	rm -rf install
	rm -f gppkg/gppkg_spec.yml
	rm -f $(PGCRYPTO_GPPKG) $(PGCRYPTO_RPM)

.PHONY: gppkg clean
