all: gppkg

BLD_TOP=../../../../../
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk
include $(BLD_TOP)/Makefile.global

OS=$(word 1,$(subst _, ,$(BLD_ARCH)))
ARCH=$(shell uname -p)

JRE_VER=1.6.0_26
JRE_REL=1
JRE_DIR=$(RELENG_TOOLS)/jre-$(JRE_VER)
JRE_RPM=jre-$(JRE_VER)-$(JRE_REL).$(ARCH).rpm

GPHDFS_VER=1.1
GPHDFS_REL=1
GPHDFS_RPM=gphdfs-$(GPHDFS_VER)-$(GPHDFS_REL).$(ARCH).rpm
GPHDFS_GPPKG=gphdfs-$(GPHDFS_VER)-$(OS)-$(ARCH).gppkg

gppkg: $(GPHDFS_GPPKG) 

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec --define "gphdfs_ver $(GPHDFS_VER)" --define "gphdfs_rel $(GPHDFS_REL)"\
										--define "jre_ver $(JRE_VER)" --define "jre_rel $(JRE_REL)"\
										--define "jre_dir $(JRE_DIR)"
	mv RPMS/$(ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS

gppkg/gppkg_spec.yml: gppkg/gppkg_spec.yml.bk
	cat $< | sed "s/#arch/$(ARCH)/g" | sed "s/#os/$(OS)/g" > $@

$(GPHDFS_GPPKG): gppkg/gppkg_spec.yml $(JRE_DIR) $(JRE_RPM) $(GPHDFS_RPM)   
	cp $(GPHDFS_RPM) gppkg/
	rm -rf gppkg/deps
	mkdir gppkg/deps
	cp $(JRE_RPM) gppkg/deps
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg/

clean:
	rm -rf RPMS SOURCES SRPMS BUILD
	rm -rf install
	rm -f gppkg/gppkg_spec.yml
	rm -f $(GPHDFS_GPPKG) $(GPHDFS_RPM)

.PHONY: gppkg clean
