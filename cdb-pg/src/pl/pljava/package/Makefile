MACHINE_ARCH=$(shell uname -p)
OS=$(shell uname -s)

JRE_VER=1.6.0_26
JRE_REL=1
JRE_DIR=$(RELENG_TOOLS)/jre-$(JRE_VER)
JRE_RPM=jre-$(JRE_VER)-$(JRE_REL).$(MACHINE_ARCH).rpm

PLJAVA_VER=1.0
PLJAVA_REL=1
PLJAVA_RPM=pljava-$(PLJAVA_VER)-$(PLJAVA_REL).$(MACHINE_ARCH).rpm
PLJAVA_GPPKG=pljava-$(PLJAVA_VER)-$(OS)-$(MACHINE_ARCH).gppkg

all: $(PLJAVA_GPPKG)

BLD_TOP=../../../../..
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec --define "jre_ver $(JRE_VER)" --define "pljava_ver $(PLJAVA_VER)" \
										--define "jre_rel $(JRE_REL)" --define "pljava_rel $(PLJAVA_REL)" \
										--define "jre_dir $(JRE_DIR)"
	mv RPMS/$(MACHINE_ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS

$(PLJAVA_GPPKG): $(JRE_DIR) $(JRE_RPM) $(PLJAVA_RPM)
	sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml 
	sed -i".bk" "s/#os/$(OS)/g" gppkg/gppkg_spec.yml
	rm -f gppkg/*.bk
	cp $(PLJAVA_RPM) gppkg/ 
	rm -rf gppkg/deps 
	mkdir gppkg/deps 
	cp $(JRE_RPM) gppkg/deps
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg 

clean:
	rm -rf RPMS SRPMS SOURCES BUILD
	rm -rf gppkg/deps
	rm -rf install
	rm -rf $(PLJAVA_GPPKG) $(JRE_RPM) $(PLJAVA_RPM)

.PHONY: clean
