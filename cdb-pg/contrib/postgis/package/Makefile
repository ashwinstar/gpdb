MACHINE_ARCH=$(shell uname -p)
OS=$(shell uname -s)

GEOS_VER=3.2.2
GEOS_REL=1
GEOS_DIR=$(RELENG_TOOLS)/geos-$(GEOS_VER)
GEOS_CONFIG=$(GEOS_DIR)/bin/geos-config
GEOS_RPM=geos-$(GEOS_VER)-$(GEOS_REL).$(MACHINE_ARCH).rpm

PROJ_VER=4.7.0
PROJ_REL=1
PROJ_DIR=$(RELENG_TOOLS)/proj-$(PROJ_VER)
PROJ_RPM=proj-$(PROJ_VER)-$(PROJ_REL).$(MACHINE_ARCH).rpm

POSTGIS_VER=1.4.2
POSTGIS_REL=1
POSTGIS_RPM=postgis-$(POSTGIS_VER)-$(POSTGIS_REL).$(MACHINE_ARCH).rpm
POSTGIS_GPPKG=postgis-$(POSTGIS_VER)-$(OS)-$(MACHINE_ARCH).gppkg

all: $(POSTGIS_GPPKG)

BLD_TOP=../../../../
GPMGMT=$(BLD_TOP)/gpMgmt
include $(BLD_TOP)/releng/tools.mk

# e.g. 
# postgis-1.4.2-1-x86_64.rpm
RPM_ARGS=$(subst -, ,$*)
RPM_NAME=$(word 1,$(RPM_ARGS))
%.rpm:
	mkdir RPMS SOURCES BUILD SRPMS
	rpmbuild -bb SPECS/$(RPM_NAME).spec --define "geos_dir $(GEOS_DIR)" --define "proj_dir $(PROJ_DIR)" \
										--define "geos_ver $(GEOS_VER)" --define "proj_ver $(PROJ_VER)"
	mv RPMS/$(MACHINE_ARCH)/$*.rpm .
	rm -rf RPMS SOURCES BUILD SRPMS
	
$(POSTGIS_GPPKG): $(GEOS_DIR) $(PROJ_DIR) $(POSTGIS_RPM) $(PROJ_RPM) $(GEOS_RPM)
	sed -i".bk" "s/#arch/$(MACHINE_ARCH)/g" gppkg/gppkg_spec.yml
	sed -i".bk" "s/#os/$(OS)/g" gppkg/gppkg_spec.yml
	rm -f gppkg/*.bk
	cp $(POSTGIS_RPM) gppkg/ 
	rm -rf gppkg/deps 
	mkdir gppkg/deps 
	cp $(GEOS_RPM) gppkg/deps 
	cp $(PROJ_RPM) gppkg/deps 
	source $(INSTLOC)/greenplum_path.sh && $(GPMGMT)/bin/gppkg -b gppkg 

clean:
	rm -rf RPMS SOURCES BUILD SRPMS
	rm -rf gppkg/deps
	rm -rf geos
	rm -rf proj
	rm -f $(POSTGIS_GPPKG) $(POSTGIS_RPM) $(GEOS_RPM) $(PROJ_RPM)

.PHONY: clean
